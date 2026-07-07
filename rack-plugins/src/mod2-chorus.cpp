#include "plugin.hpp"
#include <ChorusCore.h>  // Shared chorus DSP (also used by mod2-chorus firmware)

/*
	Chorus — Juno-style chorus + string-machine ensemble.

	Port of firmwares/mod2-chorus/mod2-chorus.ino (HAGIWO Mod2, RP2350).

	Mirrors the Mod2 hardware: 3 pots, 1 push button, 1 PWM LED, and the Mod2
	jack set (IN1, IN2, CV, OUT). For the Chorus firmware:
		POT1 (A0) -> LFO rate (0.1 - 8 Hz, exponential taper)
		POT2 (A1) -> Depth (modulation excursion)
		POT3 (A2) -> unavailable on hardware (pin doubles as the audio input)
		BUTTON    -> mode: Chorus I / Chorus II / Ensemble
		LED       -> breathes at the LFO rate
		IN1       -> (spare on hardware; no port here)
		IN2       -> bypass gate (>1 V = dry)
		CV        -> AUDIO INPUT (sampled at the audio rate on hardware)
		OUT       -> audio output

	Deviations from hardware: the firmware's wet/dry lives on a shift layer
	(hold BUTTON + turn POT1) because POT3's pin is the audio input; here the
	physically-present-but-dead third knob becomes a proper Mix control. The
	delay arena is sized for Rack's engine rate. The mode persists in the
	patch (firmware: flash).
*/

struct Chorus : Module {
	enum ParamId {
		RATE_PARAM,
		DEPTH_PARAM,
		MIX_PARAM,
		MODE_PARAM,   // momentary button — cycles Chorus I / II / Ensemble
		PARAMS_LEN
	};
	enum InputId {
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

	// Chorus state lives in the shared core (same DSP as the firmware).
	sc::ChorusCore core;
	std::vector<int16_t> arena;

	dsp::BooleanTrigger modeButton;

	Chorus() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(RATE_PARAM, 0.f, 1.f, 0.35f, "LFO rate (0.1 → 8 Hz)");
		configParam(DEPTH_PARAM, 0.f, 1.f, 0.5f, "Depth", "%", 0.f, 100.f);
		configParam(MIX_PARAM, 0.f, 1.f, 0.5f, "Wet/dry mix", "%", 0.f, 100.f);
		configButton(MODE_PARAM, "Mode (Chorus I / Chorus II / Ensemble)");

		configInput(BYPASS_INPUT, "IN2 bypass gate (>1 V = dry)");
		configInput(AUDIO_INPUT, "Audio");
		configOutput(AUDIO_OUTPUT, "Audio");

		configBypass(AUDIO_INPUT, AUDIO_OUTPUT);
		updateArena(APP->engine->getSampleRate());
	}

	// The int16 arena mirrors the firmware's SRAM buffer, sized for the
	// engine rate so the tap excursion is identical at any sample rate.
	void updateArena(float fs) {
		const uint32_t n = sc::chorusArenaSamples(fs);
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
		core.mode = sc::CHORUS_I;
	}

	// The mode persists with the patch (the firmware stores it in flash).
	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "chorusMode", json_integer(core.mode));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* modeJ = json_object_get(rootJ, "chorusMode");
		if (modeJ)
			core.mode = (uint8_t)clamp((int)json_integer_value(modeJ), 0,
			                           sc::CHORUS_MODE_COUNT - 1);
	}

	void process(const ProcessArgs& args) override {
		if (modeButton.process(params[MODE_PARAM].getValue() > 0.5f))
			core.mode = (core.mode + 1) % sc::CHORUS_MODE_COUNT;

		// Same pot mappings as the firmware.
		core.rateHz = sc::chorusRateHz(params[RATE_PARAM].getValue());
		core.depth = params[DEPTH_PARAM].getValue();
		core.wet = params[MIX_PARAM].getValue();

		// +/-5 V -> -1..1 through the shared core and back.
		const float in = inputs[AUDIO_INPUT].getVoltage() / 5.f;
		float out = core.process(in, args.sampleTime);

		// IN2 bypass gate: >1 V passes the dry input (firmware behaviour).
		if (inputs[BYPASS_INPUT].getVoltage() > 1.f)
			out = in;

		outputs[AUDIO_OUTPUT].setVoltage(clamp(out, -1.f, 1.f) * 5.f);

		// LED breathes at the LFO rate, as on hardware.
		lights[LFO_LIGHT].setBrightness(core.ledLevel());
	}
};

struct ChorusWidget : ModuleWidget {
	ChorusWidget(Chorus* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/mod2-chorus.svg")));
		// 4 HP Mod1/Mod2 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.03f, 21.7f)), module, Chorus::RATE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 40.06f)), module, Chorus::DEPTH_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 58.42f)), module, Chorus::MIX_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, Chorus::MODE_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, Chorus::LFO_LIGHT));

		// IN1 is spare on this firmware, so its jack position stays empty.
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.3f)), module, Chorus::BYPASS_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, Chorus::AUDIO_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, Chorus::AUDIO_INPUT));
	}
};

Model* modelChorus = createModel<Chorus, ChorusWidget>("mod2-chorus");
