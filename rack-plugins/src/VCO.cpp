#include "plugin.hpp"
#include <VcoCore.h>  // Shared VCO voice (also used by mod2-vco firmware)

/*
	VCO — Six-waveform oscillator with PolyBLEP anti-aliasing.

	Port of firmwares/mod2-vco/mod2-vco.ino (HAGIWO Mod2, RP2350).

	Mirrors the Mod2 hardware: 3 pots, 1 push button, 1 LED, and the Mod2
	jack set (IN1, IN2, CV, OUT). For the VCO firmware:
		POT1   -> Wave select (Sine/Tri/Square/Saw/FM-4x/FM-2x)
		POT2   -> Coarse tune (320–410 Hz base frequency)
		POT3   -> FM depth (firmware had this hardcoded at 2.0 rad;
		          here exposed as a knob, 0–4 rad, default 2.0)
		BUTTON -> Octave shift: cycles 0 / +1 / +2 / +3
		LED    -> OCT indicator (brightness = octave level)
		IN1    -> unused (sync candidate)
		IN2    -> unused
		CV     -> 1V/Oct pitch
		OUT    -> audio output

	The firmware uses a negative-slope CV (hardware inversion on A2). This
	port uses standard non-inverted 1V/Oct so it tracks sequencers as expected.
	Base tune range (320–410 Hz) and octave-shift behaviour match exactly.
*/

struct VCO : Module {
	enum ParamId {
		WAVE_PARAM,
		TUNE_PARAM,
		FM_PARAM,
		OCT_PARAM,   // momentary button
		PARAMS_LEN
	};
	enum InputId {
		IN1_INPUT,   // unused (sync candidate)
		IN2_INPUT,   // unused
		CV_INPUT,    // 1V/Oct pitch
		INPUTS_LEN
	};
	enum OutputId {
		AUDIO_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		OCT_LIGHT,
		LIGHTS_LEN
	};

	// Base tuning range — matches firmware A1 pot (TUNE_MIN_HZ + TUNE_RANGE_HZ).
	static constexpr float TUNE_MIN_HZ   = 320.0f;
	static constexpr float TUNE_RANGE_HZ =  90.0f;

	// Voice state lives in the shared core (same synthesis as the firmware).
	sc::VcoCore core;

	dsp::BooleanTrigger octButton;
	int octShift = 0;

	VCO() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(WAVE_PARAM, 0.f, 1.f, 0.f,  "Waveform");
		configParam(TUNE_PARAM, 0.f, 1.f, 0.5f, "Coarse tune (320–410 Hz)");
		configParam(FM_PARAM,   0.f, 1.f, 0.5f, "FM depth (0–4 rad)");
		configButton(OCT_PARAM, "Octave shift (0/+1/+2/+3)");

		configInput(IN1_INPUT, "IN1 (sync — unused)");
		configInput(IN2_INPUT, "IN2 (unused)");
		configInput(CV_INPUT,  "1V/Oct pitch");
		configOutput(AUDIO_OUTPUT, "Audio");
	}

	void onReset() override {
		core.reset();
		octShift = 0;
	}

	void process(const ProcessArgs& args) override {
		// Octave button: cycle 0 → 1 → 2 → 3 (matches firmware button logic).
		if (octButton.process(params[OCT_PARAM].getValue() > 0.5f))
			octShift = (octShift + 1) & 3;

		// Wave select: same 6 ADC thresholds as the firmware A0 pot.
		core.waveIndex = sc::vcoWaveSelect(params[WAVE_PARAM].getValue());

		// FM depth: knob 0..1 → 0..4 rad; default 0.5 → 2.0 rad (firmware default).
		core.fmAmount = params[FM_PARAM].getValue() * 4.0f;

		// Base frequency from TUNE knob (matches firmware A1 range 320–410 Hz).
		const float baseFreq = TUNE_MIN_HZ + TUNE_RANGE_HZ * params[TUNE_PARAM].getValue();

		// 1V/Oct: standard non-inverted (firmware used negative slope due to hardware
		// wiring; this port converges to standard so it tracks sequencers correctly).
		const float cv = inputs[CV_INPUT].getVoltage();
		core.freq = clamp(baseFreq * std::pow(2.0f, octShift + cv), 1.0f, 20000.0f);

		// One sample from the shared core: audio in -1..+1 → ±5 V (Vpp 10 V).
		outputs[AUDIO_OUTPUT].setVoltage(core.process(args.sampleTime) * 5.0f);

		// OCT light: brightness tracks current octave shift level (0..3 → 0..1).
		lights[OCT_LIGHT].setBrightness(octShift / 3.0f);
	}
};

struct VCOWidget : ModuleWidget {
	VCOWidget(VCO* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/VCO.svg")));
		// 4 HP Mod1/Mod2 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.03f, 21.7f)), module, VCO::WAVE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 40.06f)), module, VCO::TUNE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 58.42f)), module, VCO::FM_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, VCO::OCT_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, VCO::OCT_LIGHT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, VCO::IN1_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.3f)), module, VCO::IN2_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, VCO::AUDIO_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, VCO::CV_INPUT));
	}
};

Model* modelVCO = createModel<VCO, VCOWidget>("VCO");
