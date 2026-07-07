#include "plugin.hpp"
#include <PhaserCore.h>  // Shared phaser DSP (also used by mod2-phaser firmware)

/*
	Phaser — 4/6/8-stage allpass phaser.

	Port of firmwares/mod2-phaser/mod2-phaser.ino (HAGIWO Mod2, RP2350).

	Mirrors the Mod2 hardware: 3 pots, 1 push button, 1 PWM LED, and the Mod2
	jack set (IN1, IN2, CV, OUT). For the Phaser firmware:
		POT1 (A0) -> LFO rate (0.02 - 8 Hz; full CCW = manual sweep via POT2)
		POT2 (A1) -> Feedback / resonance (notch depth -> vowely peaks)
		POT3 (A2) -> unavailable on hardware (pin doubles as the audio input)
		BUTTON    -> stage count: 4 / 6 / 8
		LED       -> follows the LFO
		IN1       -> LFO retrigger
		IN2       -> bypass gate (>1 V = dry)
		CV        -> AUDIO INPUT (sampled at the audio rate on hardware)
		OUT       -> audio output

	Deviations from hardware: the firmware's wet/dry lives on a shift layer
	(hold BUTTON + turn POT1) because POT3's pin is the audio input; here the
	physically-present-but-dead third knob becomes a proper Mix control
	(50% = the classic phaser null). The second shift layer (hold BUTTON +
	turn POT2 = sweep depth) has no spare knob at all, so Depth lives in the
	context menu. In manual mode (Rate fully CCW) the Resonance knob sweeps
	by hand and the feedback amount freezes, exactly as on hardware. The
	stage count persists in the patch (firmware: flash).
*/

struct Phaser : Module {
	enum ParamId {
		RATE_PARAM,
		FEEDBACK_PARAM,
		MIX_PARAM,
		STAGES_PARAM, // momentary button — cycles 4 / 6 / 8 stages
		DEPTH_PARAM,  // context-menu slider (hardware: BUTTON + POT2)
		PARAMS_LEN
	};
	enum InputId {
		RETRIG_INPUT, // IN1 — LFO retrigger
		BYPASS_INPUT, // IN2 — bypass gate (>1 V = dry)
		AUDIO_INPUT,  // CV jack — audio in
		INPUTS_LEN
	};
	enum OutputId {
		AUDIO_OUTPUT, // OUT
		OUTPUTS_LEN
	};
	enum LightId {
		LFO_LIGHT,
		LIGHTS_LEN
	};

	// Phaser state lives in the shared core (same DSP as the firmware).
	sc::PhaserCore core;

	dsp::SchmittTrigger retrigTrigger;
	dsp::BooleanTrigger stagesButton;

	Phaser() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(RATE_PARAM, 0.f, 1.f, 0.4f, "LFO rate (0.02 → 8 Hz; full CCW = manual)");
		configParam(FEEDBACK_PARAM, 0.f, 1.f, 0.f, "Resonance", "%", 0.f, 100.f);
		configParam(MIX_PARAM, 0.f, 1.f, 0.5f, "Wet/dry mix (50% = classic null)", "%", 0.f, 100.f);
		configButton(STAGES_PARAM, "Stages (4 / 6 / 8)");
		configParam(DEPTH_PARAM, 0.f, 1.f, 0.8f, "Sweep depth", "%", 0.f, 100.f);

		configInput(RETRIG_INPUT, "IN1 LFO retrigger");
		configInput(BYPASS_INPUT, "IN2 bypass gate (>1 V = dry)");
		configInput(AUDIO_INPUT, "Audio");
		configOutput(AUDIO_OUTPUT, "Audio");

		configBypass(AUDIO_INPUT, AUDIO_OUTPUT);
	}

	void onReset() override {
		core.reset();
		core.stageSel = sc::PHASER_4;
	}

	// The stage count persists with the patch (the firmware stores it in flash).
	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "phaserStages", json_integer(core.stageSel));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* stagesJ = json_object_get(rootJ, "phaserStages");
		if (stagesJ)
			core.stageSel = (uint8_t)clamp((int)json_integer_value(stagesJ), 0,
			                               sc::PHASER_STAGES_COUNT - 1);
	}

	void process(const ProcessArgs& args) override {
		if (stagesButton.process(params[STAGES_PARAM].getValue() > 0.5f))
			core.stageSel = (core.stageSel + 1) % sc::PHASER_STAGES_COUNT;

		// IN1: restart the LFO sweep.
		if (retrigTrigger.process(inputs[RETRIG_INPUT].getVoltage(), 0.1f, 1.f))
			core.retrigger();

		// Same pot mappings as the firmware — including manual mode: with
		// Rate fully CCW the Resonance knob sweeps by hand and the feedback
		// amount freezes at its last value.
		const float ratePot = params[RATE_PARAM].getValue();
		const float fbPot = params[FEEDBACK_PARAM].getValue();
		core.manual = sc::phaserManual(ratePot);
		core.rateHz = sc::phaserRateHz(ratePot);
		if (core.manual)
			core.manualPos = fbPot;
		else
			core.feedback = sc::phaserFeedback(fbPot);
		core.depth = params[DEPTH_PARAM].getValue();
		core.wet = params[MIX_PARAM].getValue();

		// +/-5 V -> -1..1 through the shared core and back.
		const float in = inputs[AUDIO_INPUT].getVoltage() / 5.f;
		float out = core.process(in, args.sampleTime);

		// IN2 bypass gate: >1 V passes the dry input (firmware behaviour).
		if (inputs[BYPASS_INPUT].getVoltage() > 1.f)
			out = in;

		outputs[AUDIO_OUTPUT].setVoltage(clamp(out, -1.f, 1.f) * 5.f);

		// LED follows the LFO, as on hardware.
		lights[LFO_LIGHT].setBrightness(core.ledLevel());
	}
};

struct PhaserWidget : ModuleWidget {
	PhaserWidget(Phaser* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/mod2-phaser.svg")));
		// 4 HP Mod1/Mod2 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.03f, 21.7f)), module, Phaser::RATE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 40.06f)), module, Phaser::FEEDBACK_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 58.42f)), module, Phaser::MIX_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, Phaser::STAGES_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, Phaser::LFO_LIGHT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, Phaser::RETRIG_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.3f)), module, Phaser::BYPASS_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, Phaser::AUDIO_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, Phaser::AUDIO_INPUT));
	}

	// Depth is the firmware's second shift layer (BUTTON + POT2); with no
	// spare knob on the 4 HP panel it lives here in the context menu.
	void appendContextMenu(Menu* menu) override {
		Phaser* module = getModule<Phaser>();
		menu->addChild(new MenuSeparator);
		ui::Slider* slider = new ui::Slider;
		slider->quantity = module->paramQuantities[Phaser::DEPTH_PARAM];
		slider->box.size.x = 200.f;
		menu->addChild(slider);
	}
};

Model* modelPhaser = createModel<Phaser, PhaserWidget>("mod2-phaser");
