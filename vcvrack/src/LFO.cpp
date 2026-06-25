#include "plugin.hpp"
#include <LfoCore.h>  // Shared LFO core (also used by mod1-lfo firmware)

/*
	LFO — multi-waveform low-frequency oscillator.

	Port of firmwares/mod1-lfo/mod1-lfo.ino (Hagiwo Mod1).

	Six waveforms selectable via WAVE knob + CV:
		sine / triangle / square / sawtooth / reverse-saw / max (DC)

	Mirrors the Mod1 hardware: 3 pots, 1 push button, 1 PWM LED, and
	4 flexible jacks (F1..F4). For the LFO firmware:
		POT1  -> Rate (frequency)
		POT2  -> Wave (waveform select)
		POT3  -> Level (output amplitude)
		BUTTON-> toggles frequency range 1x / 10x (persisted to EEPROM)
		LED   -> follows LFO output brightness
		F1    -> frequency CV input (sums with POT1)
		F2    -> waveform CV input  (sums with POT2)
		F3    -> level CV input     (sums with POT3)
		F4    -> LFO output (bipolar ±5V)

	Behavior note: waveforms are computed closed-form (sinf, triangle formula,
	square, saw, rev-saw) rather than from the firmware's six 1024-byte PROGMEM
	lookup tables. Shapes are mathematically identical; no LUT quantization
	artifacts. See LfoCore.h for full behavioral notes.

	Frequency ranges (RANGE toggle):
		1x:  ~0.010 Hz (rate=0) — ~1.508 Hz (rate=1)
		10x: ~0.010 Hz (rate=0) — ~14.99 Hz (rate=1)
*/

struct LFO : Module {
	enum ParamId {
		RATE_PARAM,
		WAVE_PARAM,
		LEVEL_PARAM,
		RANGE_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		F1_INPUT,   // frequency CV
		F2_INPUT,   // waveform CV
		F3_INPUT,   // level CV
		INPUTS_LEN
	};
	enum OutputId {
		F4_OUTPUT,  // LFO out (bipolar ±5V)
		OUTPUTS_LEN
	};
	enum LightId {
		LED_LIGHT,
		LIGHTS_LEN
	};

	// LFO state lives in the shared core (same phase init as the firmware).
	sc::LfoVoice lfo;

	LFO() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(RATE_PARAM,  0.f, 1.f, 0.5f, "Rate");
		configParam(WAVE_PARAM,  0.f, 1.f, 0.0f, "Waveform (sine/tri/squ/saw/rsaw/DC)");
		configParam(LEVEL_PARAM, 0.f, 1.f, 1.0f, "Level");
		configSwitch(RANGE_PARAM, 0.f, 1.f, 0.f, "Freq range",
		             {"1x (0.01–1.5 Hz)", "10x (0.01–15 Hz)"});

		configInput(F1_INPUT, "F1 frequency CV");
		configInput(F2_INPUT, "F2 waveform CV");
		configInput(F3_INPUT, "F3 level CV");
		configOutput(F4_OUTPUT, "F4 LFO");
	}

	void onReset() override {
		lfo.reset();
	}

	void process(const ProcessArgs& args) override {
		// RANGE toggle: mirrors firmware's EEPROM-stored freqRange (1 or 10).
		const int freqRange = (params[RANGE_PARAM].getValue() > 0.5f) ? 10 : 1;

		// Sum pot (0..1) + CV (scaled 0..5V → 0..1), clamped to 0..1.
		// Matches firmware's addClamp1023(pot_adc, cv_adc) / 1023.
		const float rate01  = clamp(params[RATE_PARAM].getValue()
		                          + inputs[F1_INPUT].getVoltage() / 5.f, 0.f, 1.f);
		const float waveNorm = clamp(params[WAVE_PARAM].getValue()
		                           + inputs[F2_INPUT].getVoltage() / 5.f, 0.f, 1.f);
		const float levelNorm = clamp(params[LEVEL_PARAM].getValue()
		                            + inputs[F3_INPUT].getVoltage() / 5.f, 0.f, 1.f);

		const uint8_t wave = sc::lfoSelectWave(waveNorm);
		const float freq   = sc::lfoMapFreq(rate01, freqRange);

		float ledPhase = 0.f;
		const float out = lfo.process(args.sampleTime, freq, wave, levelNorm, ledPhase);

		// Bipolar ±5V output (matches firmware's 0..255 PWM centered at 127).
		outputs[F4_OUTPUT].setVoltage(out * 5.f);

		// LED brightness follows the LFO output (0 = off, 1 = full).
		lights[LED_LIGHT].setBrightness((out + 1.f) * 0.5f);
	}
};

struct LFOWidget : ModuleWidget {
	LFOWidget(LFO* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/LFO.svg")));
		// 4 HP Mod1/Mod2 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.03f, 21.7f)), module, LFO::RATE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 40.06f)), module, LFO::WAVE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 58.42f)), module, LFO::LEVEL_PARAM));
		addParam(createParamCentered<VCVLatch>(mm2px(Vec(5.19f, 78.57f)), module, LFO::RANGE_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, LFO::LED_LIGHT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, LFO::F1_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.3f)), module, LFO::F2_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, LFO::F3_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, LFO::F4_OUTPUT));
	}
};

Model* modelLFO = createModel<LFO, LFOWidget>("LFO");
