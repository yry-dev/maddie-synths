// WIP: Claude-generated maddie synths original (no upstream Hagiwo/Rob
// Scape firmware). Excluded from the VCV Rack build -- see WIP_SOURCES in
// the Makefile and the WIP block in plugin.cpp for how to re-enable it.
#include "plugin.hpp"
#include <WavefolderCore.h>  // Shared folder DSP (also used by mod2-wavefolder firmware)

/*
	Wavefolder — digital West-Coast (Serge / Buchla-style) folder.

	Port of firmwares/mod2-wavefolder/mod2-wavefolder.ino (HAGIWO Mod2, RP2350).

	Mirrors the Mod2 hardware: 3 pots, 1 push button, 1 PWM LED, and the Mod2
	jack set (IN1, IN2, CV, OUT). For the Wavefolder firmware:
		POT1 (A0) -> Fold amount (1× → 20× pre-gain into the folder)
		POT2 (A1) -> Symmetry / offset (pre-fold DC bias → even harmonics)
		POT3 (A2) -> unavailable on hardware (pin doubles as the audio input)
		BUTTON    -> fold curve: Reflect / Sine / Cascade
		LED       -> fold density
		IN1       -> (spare on hardware; no port here)
		IN2       -> bypass gate (>1 V = dry)
		CV        -> AUDIO INPUT (sampled at the audio rate on hardware)
		OUT       -> audio output

	Deviations from hardware: the firmware's wet/dry lives on a shift layer
	(hold BUTTON + turn POT1) because POT3's pin is the audio input; here the
	physically-present-but-dead third knob becomes a proper Mix control. The
	firmware's second shift layer (BUTTON + POT2 → post-fold low-pass) has no
	free knob slot on the 3-pot panel, so the Rack port leaves the post-fold
	tone wide open (the folder's brightest setting); the curve persists in the
	patch (firmware: flash).
*/

struct Wavefolder : Module {
	enum ParamId {
		FOLD_PARAM,
		SYMM_PARAM,
		MIX_PARAM,
		MODE_PARAM,   // momentary button — cycles the fold curve
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
		FOLD_LIGHT,
		LIGHTS_LEN
	};

	// Fold state lives in the shared core (same DSP as the firmware).
	sc::WavefolderCore core;

	dsp::BooleanTrigger modeButton;

	Wavefolder() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(FOLD_PARAM, 0.f, 1.f, 0.3f, "Fold amount (1× → 20×)");
		configParam(SYMM_PARAM, 0.f, 1.f, 0.5f, "Symmetry / offset");
		configParam(MIX_PARAM, 0.f, 1.f, 1.f, "Wet/dry mix", "%", 0.f, 100.f);
		configButton(MODE_PARAM, "Fold curve (Reflect / Sine / Cascade)");

		configInput(BYPASS_INPUT, "IN2 bypass gate (>1 V = dry)");
		configInput(AUDIO_INPUT, "Audio");
		configOutput(AUDIO_OUTPUT, "Audio");

		configBypass(AUDIO_INPUT, AUDIO_OUTPUT);

		// Post-fold tone is wide open in Rack (no free knob slot); see header.
		core.toneHz = sc::wavefolderToneHz(1.f);
	}

	void onReset() override {
		core.mode = sc::WAVEFOLDER_REFLECT;
		core.toneHz = sc::wavefolderToneHz(1.f);
		core.reset();
	}

	// The fold curve persists with the patch (the firmware stores it in flash).
	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "foldMode", json_integer(core.mode));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* modeJ = json_object_get(rootJ, "foldMode");
		if (modeJ)
			core.mode = (uint8_t)clamp((int)json_integer_value(modeJ), 0,
			                           sc::WAVEFOLDER_MODE_COUNT - 1);
	}

	void process(const ProcessArgs& args) override {
		if (modeButton.process(params[MODE_PARAM].getValue() > 0.5f))
			core.setMode((core.mode + 1) % sc::WAVEFOLDER_MODE_COUNT);

		// Same pot mappings as the firmware.
		core.foldGain = sc::wavefolderFoldGain(params[FOLD_PARAM].getValue());
		core.offset = sc::wavefolderOffset(params[SYMM_PARAM].getValue());
		core.wet = params[MIX_PARAM].getValue();

		// +/-5 V -> -1..1 through the shared core and back.
		const float in = inputs[AUDIO_INPUT].getVoltage() / 5.f;
		float out = core.process(in, args.sampleTime);

		// IN2 bypass gate: >1 V passes the dry input (firmware behaviour).
		if (inputs[BYPASS_INPUT].getVoltage() > 1.f)
			out = in;

		outputs[AUDIO_OUTPUT].setVoltage(clamp(out, -1.f, 1.f) * 5.f);

		// LED follows fold density, as on hardware.
		lights[FOLD_LIGHT].setBrightness(core.ledLevel());
	}
};

struct WavefolderWidget : ModuleWidget {
	WavefolderWidget(Wavefolder* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/mod2-wavefolder.svg")));
		// 4 HP Mod1/Mod2 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.03f, 21.7f)), module, Wavefolder::FOLD_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 40.06f)), module, Wavefolder::SYMM_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 58.42f)), module, Wavefolder::MIX_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, Wavefolder::MODE_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, Wavefolder::FOLD_LIGHT));

		// IN1 is spare on this firmware, so its jack position stays empty.
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.3f)), module, Wavefolder::BYPASS_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, Wavefolder::AUDIO_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, Wavefolder::AUDIO_INPUT));
	}
};

Model* modelWavefolder = createModel<Wavefolder, WavefolderWidget>("mod2-wavefolder");
