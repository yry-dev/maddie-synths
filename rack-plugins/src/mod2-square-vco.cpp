#include "plugin.hpp"
#include <SquareVcoCore.h>  // Shared square-wave VCO core (also used by mod2-square-vco firmware)

/*
	SquareVCO — square-wave VCO with sine LFO vibrato and chiptune mode.

	Port of firmwares/mod2-square-vco/mod2-square-vco.ino (HAGIWO Mod2, RP2350).

	Mirrors the Mod2 hardware: 3 pots, 1 push button, 1 LED, and the Mod2 jack
	set (IN1, IN2, CV, OUT). For the Square VCO firmware:
		POT1 (A0)  -> TUNE: fine tune factor 1.0–2.0×
		POT2 (A1)  -> OCTAVE: 6-step octave selector (1, 2, 4, 8, 16, 32×)
		POT3 / CV  -> VIB: vibrato depth (firmware had this fixed at 2%; exposed here)
		BUTTON     -> chiptune mode toggle (20 Hz octave alternation)
		LED        -> chiptune mode indicator
		IN1        -> octave CV (shifts octave knob ±)
		IN2        -> vibrato depth CV
		CV (A2)    -> 1V/Oct pitch (standard non-inverted)
		OUT        -> audio output

	The firmware drives the core at ~36621 Hz; here we synthesise live at the
	host sample rate, so it is sample-rate independent.

	Convergence note: the firmware's V/Oct uses a 1024-entry lookup table that
	maps 0–3.3 V across 5 octaves (0.66 V/Oct). This port uses standard
	1V/Oct (cvMult = 2^volts) so it tracks sequencers as expected.
*/

struct SquareVCO : Module {
	enum ParamId {
		TUNE_PARAM,
		OCTAVE_PARAM,
		VIB_DEPTH_PARAM,
		CHIPTUNE_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		IN1_INPUT,   // octave CV
		IN2_INPUT,   // vibrato depth CV
		CV_INPUT,    // 1V/Oct pitch
		INPUTS_LEN
	};
	enum OutputId {
		AUDIO_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		CHIPTUNE_LIGHT,
		LIGHTS_LEN
	};

	sc::SquareVcoCore core;
	dsp::BooleanTrigger chiptuneButton;

	SquareVCO() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(TUNE_PARAM,      0.f, 1.f, 0.f,   "Tune (1.0–2.0×)");
		configParam(OCTAVE_PARAM,    0.f, 1.f, 0.f,   "Octave (1–32×)");
		configParam(VIB_DEPTH_PARAM, 0.f, 1.f, 0.4f,  "Vibrato depth");  // 0.4 → 0.4*0.05 = 0.02 = firmware default
		configButton(CHIPTUNE_PARAM, "Chiptune mode");

		configInput(IN1_INPUT,  "IN1 octave CV");
		configInput(IN2_INPUT,  "IN2 vibrato depth CV");
		configInput(CV_INPUT,   "1V/Oct");
		configOutput(AUDIO_OUTPUT, "Audio");
	}

	void onReset() override {
		core.reset();
		core.chiptuneOn = false;
	}

	void process(const ProcessArgs& args) override {
		// Chiptune toggle on button press
		if (chiptuneButton.process(params[CHIPTUNE_PARAM].getValue() > 0.5f))
			core.chiptuneOn = !core.chiptuneOn;
		lights[CHIPTUNE_LIGHT].setBrightness(core.chiptuneOn ? 1.f : 0.f);

		// Fine tune: knob 0..1 → factor 1.0..2.0
		core.freqFactor = sc::squareVcoTune(params[TUNE_PARAM].getValue());

		// Octave: knob + IN1 CV (each 0.1V ≈ one step via the same thresholds)
		const float octKnob = params[OCTAVE_PARAM].getValue();
		const float octCv   = inputs[IN1_INPUT].getVoltage() / 10.f;  // ±5V → ±0.5
		core.octaveIndex = sc::squareVcoOctaveIdx(clamp(octKnob + octCv, 0.f, 1.f));

		// Vibrato depth: knob + IN2 CV
		const float vibKnob = params[VIB_DEPTH_PARAM].getValue();
		const float vibCv   = inputs[IN2_INPUT].getVoltage() / 10.f;
		core.vibDepth = sc::squareVcoVibDepth(clamp(vibKnob + vibCv, 0.f, 1.f));

		// 1V/Oct pitch CV: standard non-inverted (cvMult = 2^volts)
		core.cvMult = std::pow(2.f, inputs[CV_INPUT].getVoltage());

		// One sample from the shared core: audio in -1..+1 → ±5 V
		outputs[AUDIO_OUTPUT].setVoltage(core.process(args.sampleTime) * 5.f);
	}
};

struct SquareVCOWidget : ModuleWidget {
	SquareVCOWidget(SquareVCO* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/mod2-square-vco.svg")));

		// 4 HP panel (19.8 mm): hole centres from the mod2-square-vco KiCad faceplate
		// (panel-local mm, scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.03f, 21.70f)), module, SquareVCO::TUNE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 40.06f)), module, SquareVCO::OCTAVE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 58.42f)), module, SquareVCO::VIB_DEPTH_PARAM));

		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, SquareVCO::CHIPTUNE_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, SquareVCO::CHIPTUNE_LIGHT));

		// Jacks: IN1 (top-left), IN2 (top-right), OUT (bottom-left), CV (bottom-right).
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, SquareVCO::IN1_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.30f)), module, SquareVCO::IN2_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, SquareVCO::AUDIO_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, SquareVCO::CV_INPUT));
	}
};

Model* modelSquareVCO = createModel<SquareVCO, SquareVCOWidget>("mod2-square-vco");
