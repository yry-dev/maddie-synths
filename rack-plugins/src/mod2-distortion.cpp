// WIP: Claude-generated maddie synths original (no upstream Hagiwo/Rob
// Scape firmware). Excluded from the VCV Rack build -- see WIP_SOURCES in
// the Makefile and the WIP block in plugin.cpp for how to re-enable it.
#include "plugin.hpp"
#include <DistortionCore.h>  // Shared drive DSP (also used by mod2-distortion firmware)

/*
	Distortion — multi-algorithm drive.

	Port of firmwares/mod2-distortion/mod2-distortion.ino (HAGIWO Mod2, RP2350).

	Mirrors the Mod2 hardware: 3 pots, 1 push button, 1 PWM LED, and the Mod2
	jack set (IN1, IN2, CV, OUT). For the Distortion firmware:
		POT1 (A0) -> Drive (1x - ~50x pre-gain, auto-compensated output level)
		POT2 (A1) -> Tone (post-shaper tilt: dark <-> bright)
		POT3 (A2) -> unavailable on hardware (pin doubles as the audio input)
		BUTTON    -> algorithm: Soft / Hard / Tube / Foldback / Fuzz
		LED       -> output level
		IN1       -> (spare on hardware; no port here)
		IN2       -> bypass gate (>1 V = dry)
		CV        -> AUDIO INPUT (sampled at the audio rate on hardware)
		OUT       -> audio output

	Deviations from hardware: the firmware's wet/dry lives on a shift layer
	(hold BUTTON + turn POT1) because POT3's pin is the audio input; here the
	physically-present-but-dead third knob becomes a proper Mix control. The
	algorithm persists in the patch (firmware: flash).
*/

struct Distortion : Module {
	enum ParamId {
		DRIVE_PARAM,
		TONE_PARAM,
		MIX_PARAM,
		MODE_PARAM,   // momentary button — cycles the algorithm
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
		OUT_LIGHT,
		LIGHTS_LEN
	};

	// Drive state lives in the shared core (same DSP as the firmware).
	sc::DistortionCore core;

	dsp::BooleanTrigger modeButton;

	Distortion() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(DRIVE_PARAM, 0.f, 1.f, 0.3f, "Drive (1× → ~50× pre-gain)");
		configParam(TONE_PARAM, 0.f, 1.f, 0.5f, "Tone (dark ← → bright)");
		configParam(MIX_PARAM, 0.f, 1.f, 1.f, "Wet/dry mix", "%", 0.f, 100.f);
		configButton(MODE_PARAM, "Algorithm (Soft / Hard / Tube / Foldback / Fuzz)");

		configInput(BYPASS_INPUT, "IN2 bypass gate (>1 V = dry)");
		configInput(AUDIO_INPUT, "Audio");
		configOutput(AUDIO_OUTPUT, "Audio");

		configBypass(AUDIO_INPUT, AUDIO_OUTPUT);
	}

	void onReset() override {
		core.reset();
		core.mode = sc::DISTORTION_SOFT;
	}

	// The algorithm persists with the patch (the firmware stores it in flash).
	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "driveMode", json_integer(core.mode));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* modeJ = json_object_get(rootJ, "driveMode");
		if (modeJ)
			core.mode = (uint8_t)clamp((int)json_integer_value(modeJ), 0,
			                           sc::DISTORTION_MODE_COUNT - 1);
	}

	void process(const ProcessArgs& args) override {
		if (modeButton.process(params[MODE_PARAM].getValue() > 0.5f))
			core.mode = (core.mode + 1) % sc::DISTORTION_MODE_COUNT;

		// Same pot mappings as the firmware.
		core.drive = sc::distortionDriveGain(params[DRIVE_PARAM].getValue());
		core.tone = params[TONE_PARAM].getValue();
		core.wet = params[MIX_PARAM].getValue();

		// +/-5 V -> -1..1 through the shared core and back.
		const float in = inputs[AUDIO_INPUT].getVoltage() / 5.f;
		float out = core.process(in, args.sampleTime);

		// IN2 bypass gate: >1 V passes the dry input (firmware behaviour).
		if (inputs[BYPASS_INPUT].getVoltage() > 1.f)
			out = in;

		outputs[AUDIO_OUTPUT].setVoltage(clamp(out, -1.f, 1.f) * 5.f);

		// LED follows the output level, as on hardware.
		lights[OUT_LIGHT].setBrightnessSmooth(std::fabs(out), args.sampleTime);
	}
};

struct DistortionWidget : ModuleWidget {
	DistortionWidget(Distortion* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/mod2-distortion.svg")));
		// 4 HP Mod1/Mod2 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.03f, 21.7f)), module, Distortion::DRIVE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 40.06f)), module, Distortion::TONE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 58.42f)), module, Distortion::MIX_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, Distortion::MODE_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, Distortion::OUT_LIGHT));

		// IN1 is spare on this firmware, so its jack position stays empty.
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.3f)), module, Distortion::BYPASS_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, Distortion::AUDIO_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, Distortion::AUDIO_INPUT));
	}
};

Model* modelDistortion = createModel<Distortion, DistortionWidget>("mod2-distortion");
