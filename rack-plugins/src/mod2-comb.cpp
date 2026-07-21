// WIP: Claude-generated maddie synths original (no upstream Hagiwo/Rob
// Scape firmware). Excluded from the VCV Rack build -- see WIP_SOURCES in
// the Makefile and the WIP block in plugin.cpp for how to re-enable it.
#include "plugin.hpp"
#include <CombCore.h>  // Shared comb DSP (also used by mod2-comb firmware)

/*
	Comb — tuned feedforward / feedback comb filter.

	Port of firmwares/mod2-comb/mod2-comb.ino (HAGIWO Mod2, RP2350).

	Mirrors the Mod2 hardware: 3 pots, 1 push button, 1 PWM LED, and the Mod2
	jack set (IN1, IN2, CV, OUT). For the Comb firmware:
		POT1 (A0) -> Tune (20 Hz - 2 kHz, semitone-quantized by default)
		POT2 (A1) -> Feedback (bipolar: CCW negative comb, CW positive)
		POT3 (A2) -> unavailable on hardware (pin doubles as the audio input)
		BUTTON    -> mode: Feedback / Feedforward / Both (nested all-pass)
		LED       -> follows the comb's resonant energy
		IN1       -> (spare on hardware; no port here)
		IN2       -> feedback kill gate (>1 V = choke the ring)
		CV        -> AUDIO INPUT (sampled at the audio rate on hardware)
		OUT       -> audio output

	Deviations from hardware: the firmware's wet/dry lives on a shift layer
	(hold BUTTON + turn POT1) because POT3's pin is the audio input; here the
	physically-present-but-dead third knob becomes a proper Mix control. The
	second shift layer (hold BUTTON + turn POT2 = damping) has no spare knob on
	the 4 HP panel, so Damping lives in the context menu, as does the firmware's
	long-press semitone-quantize toggle. The mode + quantize flag persist in the
	patch (firmware: flash).
*/

struct Comb : Module {
	enum ParamId {
		TUNE_PARAM,
		FEEDBACK_PARAM,
		MIX_PARAM,
		MODE_PARAM,   // momentary button — cycles Feedback / Feedforward / Both
		DAMP_PARAM,   // context-menu slider (hardware: BUTTON + POT2)
		PARAMS_LEN
	};
	enum InputId {
		FBKILL_INPUT, // IN2 — feedback kill gate (>1 V = choke)
		AUDIO_INPUT,  // CV jack — audio in
		INPUTS_LEN
	};
	enum OutputId {
		AUDIO_OUTPUT, // OUT
		OUTPUTS_LEN
	};
	enum LightId {
		ENERGY_LIGHT,
		LIGHTS_LEN
	};

	// Comb state lives in the shared core (same DSP as the firmware).
	sc::CombCore core;
	std::vector<int16_t> arena;
	bool quantize = true;  // semitone-quantized tune (firmware: long-press toggle)

	dsp::BooleanTrigger modeButton;

	Comb() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(TUNE_PARAM, 0.f, 1.f, 0.5f, "Tune (20 Hz → 2 kHz)");
		configParam(FEEDBACK_PARAM, 0.f, 1.f, 0.5f, "Feedback (bipolar: CCW − / CW +)");
		configParam(MIX_PARAM, 0.f, 1.f, 0.5f, "Wet/dry mix", "%", 0.f, 100.f);
		configButton(MODE_PARAM, "Mode (Feedback / Feedforward / Both)");
		configParam(DAMP_PARAM, 0.f, 1.f, 0.3f, "Damping (LP in the feedback path)", "%", 0.f, 100.f);

		configInput(FBKILL_INPUT, "IN2 feedback kill gate (>1 V = choke)");
		configInput(AUDIO_INPUT, "Audio");
		configOutput(AUDIO_OUTPUT, "Audio");

		configBypass(AUDIO_INPUT, AUDIO_OUTPUT);
		updateArena(APP->engine->getSampleRate());
	}

	// The int16 arena mirrors the firmware's SRAM buffer, sized for the engine
	// rate so the lowest tuned frequency always fits the delay line.
	void updateArena(float fs) {
		const uint32_t n = sc::combArenaSamples(fs);
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
		core.mode = sc::COMB_FEEDBACK;
		quantize = true;
	}

	// Mode + quantize flag persist with the patch (the firmware stores them in
	// flash).
	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "combMode", json_integer(core.mode));
		json_object_set_new(rootJ, "quantize", json_boolean(quantize));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* modeJ = json_object_get(rootJ, "combMode");
		if (modeJ)
			core.mode = (uint8_t)clamp((int)json_integer_value(modeJ), 0,
			                           sc::COMB_MODE_COUNT - 1);
		json_t* quantJ = json_object_get(rootJ, "quantize");
		if (quantJ)
			quantize = json_boolean_value(quantJ);
	}

	void process(const ProcessArgs& args) override {
		if (modeButton.process(params[MODE_PARAM].getValue() > 0.5f))
			core.mode = (core.mode + 1) % sc::COMB_MODE_COUNT;

		// Same pot mappings as the firmware.
		core.freqHz = sc::combFreqHz(params[TUNE_PARAM].getValue(), quantize);
		core.feedback = sc::combFeedback(params[FEEDBACK_PARAM].getValue());
		core.damping = params[DAMP_PARAM].getValue();
		core.wet = params[MIX_PARAM].getValue();
		core.fbKill = inputs[FBKILL_INPUT].getVoltage() > 1.f;

		// +/-5 V -> -1..1 through the shared core and back.
		const float in = inputs[AUDIO_INPUT].getVoltage() / 5.f;
		const float out = core.process(in, args.sampleTime);
		outputs[AUDIO_OUTPUT].setVoltage(clamp(out, -1.f, 1.f) * 5.f);

		// LED follows the comb's resonant energy, as on hardware.
		lights[ENERGY_LIGHT].setBrightness(core.ledLevel());
	}
};

struct CombWidget : ModuleWidget {
	CombWidget(Comb* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/mod2-comb.svg")));
		// 4 HP Mod1/Mod2 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.03f, 21.7f)), module, Comb::TUNE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 40.06f)), module, Comb::FEEDBACK_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 58.42f)), module, Comb::MIX_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, Comb::MODE_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, Comb::ENERGY_LIGHT));

		// IN1 is spare on this firmware, so its jack position stays empty.
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.3f)), module, Comb::FBKILL_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, Comb::AUDIO_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, Comb::AUDIO_INPUT));
	}

	// Damping is the firmware's second shift layer (BUTTON + POT2) and Quantize
	// its long-press action; neither has a spare control on the 4 HP panel, so
	// both live here in the context menu.
	void appendContextMenu(Menu* menu) override {
		Comb* module = getModule<Comb>();
		menu->addChild(new MenuSeparator);
		ui::Slider* slider = new ui::Slider;
		slider->quantity = module->paramQuantities[Comb::DAMP_PARAM];
		slider->box.size.x = 200.f;
		menu->addChild(slider);
		menu->addChild(createBoolPtrMenuItem("Quantize tune to semitones", "", &module->quantize));
	}
};

Model* modelComb = createModel<Comb, CombWidget>("mod2-comb");
