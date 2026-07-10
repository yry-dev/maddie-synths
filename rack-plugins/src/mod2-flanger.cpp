#include "plugin.hpp"
#include <FlangerCore.h>  // Shared flanger DSP (also used by mod2-flanger firmware)

/*
	Flanger — swept-comb flanger.

	Port of firmwares/mod2-flanger/mod2-flanger.ino (HAGIWO Mod2, RP2350).

	Mirrors the Mod2 hardware: 3 pots, 1 push button, 1 PWM LED, and the Mod2
	jack set (IN1, IN2, CV, OUT). For the Flanger firmware:
		POT1 (A0) -> LFO rate (0.02 - 5 Hz; full CCW = manual sweep via POT2)
		POT2 (A1) -> Feedback (bipolar; centre = none; last few % scream)
		POT3 (A2) -> unavailable on hardware (pin doubles as the audio input)
		BUTTON    -> sweep shape: triangle / sine / envelope-follow
		LED       -> follows the sweep
		IN1       -> LFO retrigger
		IN2       -> bypass gate (>1 V = dry)
		CV        -> AUDIO INPUT (sampled at the audio rate on hardware)
		OUT       -> audio output

	Deviations from hardware: the firmware's wet/dry lives on a shift layer
	(hold BUTTON + turn POT1) because POT3's pin is the audio input; here the
	physically-present-but-dead third knob becomes a proper Mix control. The
	second shift layer (hold BUTTON + turn POT2 = sweep depth) has no spare
	knob at all, so Depth lives in the context menu. In manual mode (Rate
	fully CCW) the Feedback knob sweeps the comb by hand and the feedback
	amount freezes, exactly as on hardware. The shape persists in the patch
	(firmware: flash).
*/

struct Flanger : Module {
	enum ParamId {
		RATE_PARAM,
		FEEDBACK_PARAM,
		MIX_PARAM,
		SHAPE_PARAM,  // momentary button — cycles triangle / sine / env
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
		SWEEP_LIGHT,
		LIGHTS_LEN
	};

	// Flanger state lives in the shared core (same DSP as the firmware).
	sc::FlangerCore core;
	std::vector<int16_t> arena;

	dsp::SchmittTrigger retrigTrigger;
	dsp::BooleanTrigger shapeButton;

	Flanger() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(RATE_PARAM, 0.f, 1.f, 0.4f, "LFO rate (0.02 → 5 Hz; full CCW = manual)");
		configParam(FEEDBACK_PARAM, 0.f, 1.f, 0.5f, "Feedback (CCW − / CW +, centre off)");
		configParam(MIX_PARAM, 0.f, 1.f, 0.5f, "Wet/dry mix", "%", 0.f, 100.f);
		configButton(SHAPE_PARAM, "Sweep shape (triangle / sine / env-follow)");
		configParam(DEPTH_PARAM, 0.f, 1.f, 0.7f, "Sweep depth", "%", 0.f, 100.f);

		configInput(RETRIG_INPUT, "IN1 LFO retrigger");
		configInput(BYPASS_INPUT, "IN2 bypass gate (>1 V = dry)");
		configInput(AUDIO_INPUT, "Audio");
		configOutput(AUDIO_OUTPUT, "Audio");

		configBypass(AUDIO_INPUT, AUDIO_OUTPUT);
		updateArena(APP->engine->getSampleRate());
	}

	// The int16 arena mirrors the firmware's SRAM buffer, sized for the
	// engine rate so the tap excursion is identical at any sample rate.
	void updateArena(float fs) {
		const uint32_t n = sc::flangerArenaSamples(fs);
		if (arena.size() != n) {
			arena.assign(n, 0);
			core.init(arena.data(), n);
		}
	}

	void onSampleRateChange(const SampleRateChangeEvent& e) override {
		updateArena(e.sampleRate);
	}

	void onReset() override {
		core.reset();
		core.shape = sc::FLANGER_TRI;
	}

	// The shape persists with the patch (the firmware stores it in flash).
	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "flangerShape", json_integer(core.shape));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* shapeJ = json_object_get(rootJ, "flangerShape");
		if (shapeJ)
			core.shape = (uint8_t)clamp((int)json_integer_value(shapeJ), 0,
			                            sc::FLANGER_SHAPE_COUNT - 1);
	}

	void process(const ProcessArgs& args) override {
		if (shapeButton.process(params[SHAPE_PARAM].getValue() > 0.5f))
			core.shape = (core.shape + 1) % sc::FLANGER_SHAPE_COUNT;

		// IN1: restart the LFO sweep.
		if (retrigTrigger.process(inputs[RETRIG_INPUT].getVoltage(), 0.1f, 1.f))
			core.retrigger();

		// Same pot mappings as the firmware — including manual mode: with
		// Rate fully CCW the Feedback knob sweeps by hand and the feedback
		// amount freezes at its last value.
		const float ratePot = params[RATE_PARAM].getValue();
		const float fbPot = params[FEEDBACK_PARAM].getValue();
		core.manual = sc::flangerManual(ratePot);
		core.rateHz = sc::flangerRateHz(ratePot);
		if (core.manual)
			core.manualPos = fbPot;
		else
			core.feedback = sc::flangerFeedback(fbPot);
		core.depth = params[DEPTH_PARAM].getValue();
		core.wet = params[MIX_PARAM].getValue();

		// +/-5 V -> -1..1 through the shared core and back.
		const float in = inputs[AUDIO_INPUT].getVoltage() / 5.f;
		float out = core.process(in, args.sampleTime);

		// IN2 bypass gate: >1 V passes the dry input (firmware behaviour).
		if (inputs[BYPASS_INPUT].getVoltage() > 1.f)
			out = in;

		outputs[AUDIO_OUTPUT].setVoltage(clamp(out, -1.f, 1.f) * 5.f);

		// LED follows the sweep, as on hardware.
		lights[SWEEP_LIGHT].setBrightness(core.ledLevel());
	}
};

struct FlangerWidget : ModuleWidget {
	FlangerWidget(Flanger* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/mod2-flanger.svg")));
		// 4 HP Mod1/Mod2 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.03f, 21.7f)), module, Flanger::RATE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 40.06f)), module, Flanger::FEEDBACK_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 58.42f)), module, Flanger::MIX_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, Flanger::SHAPE_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, Flanger::SWEEP_LIGHT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, Flanger::RETRIG_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.3f)), module, Flanger::BYPASS_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, Flanger::AUDIO_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, Flanger::AUDIO_INPUT));
	}

	// Depth is the firmware's second shift layer (BUTTON + POT2); with no
	// spare knob on the 4 HP panel it lives here in the context menu.
	void appendContextMenu(Menu* menu) override {
		Flanger* module = getModule<Flanger>();
		menu->addChild(new MenuSeparator);
		ui::Slider* slider = new ui::Slider;
		slider->quantity = module->paramQuantities[Flanger::DEPTH_PARAM];
		slider->box.size.x = 200.f;
		menu->addChild(slider);
	}
};

Model* modelFlanger = createModel<Flanger, FlangerWidget>("mod2-flanger");
