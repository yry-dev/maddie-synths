#include "plugin.hpp"

/*
	Tiny Tides — Mutable Instruments Tides (tides2) tidal wave generator:
	looping slope / LFO / oscillator with shape, slope and smoothness.

	Port of firmwares/mod2-tides/mod2-tides.ino (HAGIWO Mod2, RP2350).

	Mirrors the Mod2 hardware: 3 pots, 1 push button, 1 LED, and the Mod2
	jack set. For the Tiny Tides firmware:
		POT1    -> Shape (waveform shape)
		POT2    -> Slope (falling -> rising ramp)
		POT3    -> Frequency (20–2000 Hz)
		BUTTON  -> short press cycles output mode
		           (Gates / Amplitudes / Phases / Frequencies)
		LED     -> feedback (here: output level)
		TRIGGER -> trigger / gate input (IN1)
		SMOOTH  -> smoothness (IN2)
		FREQ    -> frequency CV (shared with POT3 on hardware)
		OUT     -> audio output (tides OUT1 / channel 0)

	Divergences from the firmware:
	 - Hardware shares A2 between POT3 and the Freq jack; here the knob sets a
	   base frequency and the jack is standard 1V/Oct (like the mod2-vco port).
	 - Hardware SMOOTH is a digital gate (LOW=0.2 / HIGH=0.8 smoothness); here
	   it is a 0–10 V CV mapped to 0..1, defaulting to 0.5 unpatched.
	 - The hardware long-press (cycle ramp mode) doesn't map well to a Rack
	   button, so ramp mode lives in the context menu (with output mode too).
	 - Range (Audio / Control) is a context-menu extension: the firmware pins
	   RANGE_AUDIO; Control turns the module into the classic slow tidal LFO
	   (the frequency knob is divided by 128, giving ~0.16–15.6 Hz).

	Build note: depends on the EXTERNAL Mutable Instruments TIDES + STMLIB
	libraries (poetaster/arduinoMI packaging), deliberately not vendored in
	this repo — see README.md "MOD2 Braids / Tides" for the install steps.
	The Makefile picks them up via MI_LIB_DIR (default: the Arduino libraries
	folder); CI fetches them at the commits pinned in .github/mi-libs.env. To
	build without them, exclude this module the usual three-place way
	(WIP_SOURCES + plugin.{hpp,cpp} + the root plugin.json).

	License:
	MIT License, Copyright (c) 2026 Madelyn Yeary — see rack-plugins/LICENSE.md.
	Wraps the Mutable Instruments tides2 DSP, Copyright (c) Émilie Gillet (MIT
	License), compiled from the external poetaster/arduinoMI packaging rather
	than vendored here. (The firmware wrapper this mirrors is GPLv3 because it
	derives from Volker Boehm's mi_Ugens; this file calls the MIT-licensed MI
	sources directly and shares no code with it.)
*/

#if !__has_include(<tides2/poly_slope_generator.h>)
#error "mod2-tides needs the external Mutable Instruments TIDES/STMLIB libraries (poetaster/arduinoMI) — see README.md 'MOD2 Braids / Tides'. Install them (MI_LIB_DIR in rack-plugins/Makefile), or exclude this module via WIP_SOURCES + plugin.{hpp,cpp} + the root plugin.json."
#endif

// TEST is Mutable's own desktop-build switch: it turns off embedded-only
// attributes in the stmlib headers (IN_RAM's ELF ".ramtext" section is invalid
// for Mach-O/PE). Audible Instruments builds the same sources the same way.
#define TEST
#include <STMLIB.h>
#include <TIDES.h>
// The library bodies (lookup tables + out-of-line definitions) are compiled
// exactly once, in src/mi-libs.cpp — this file uses only the headers.

struct TinyTides : Mod2Module {
	enum ParamId {
		SHAPE_PARAM,
		SLOPE_PARAM,
		FREQ_PARAM,
		MODE_PARAM,   // momentary button: cycle output mode
		PARAMS_LEN
	};
	enum InputId {
		TRIG_INPUT,    // trigger / gate
		SMOOTH_INPUT,  // smoothness CV (0-10 V)
		CV_INPUT,      // 1V/Oct frequency
		INPUTS_LEN
	};
	enum OutputId {
		AUDIO_OUTPUT,  // tides OUT1 (channel 0)
		OUTPUTS_LEN
	};
	enum LightId {
		FEEDBACK_LIGHT,
		LIGHTS_LEN
	};

	// Frequency knob range — matches firmware POT3 (20 Hz + 0..1980 Hz).
	static constexpr float FREQ_MIN_HZ   =   20.0f;
	static constexpr float FREQ_RANGE_HZ = 1980.0f;
	// Control (LFO) range divides the knob frequency down to ~0.16–15.6 Hz.
	static constexpr float CONTROL_DIV = 128.0f;

	// Same block size as the firmware; parameters interpolate across a block
	// inside Render(), so one block is also the control granularity (~0.3 ms).
	static constexpr int BLOCK = 16;

	tides::PolySlopeGenerator generator;
	tides::PolySlopeGenerator::OutputSample out[BLOCK] = {};
	stmlib::GateFlags previousGateFlags = stmlib::GATE_FLAG_LOW;
	int blockPos = 0;

	// Firmware boot defaults: Amplitudes / Looping / Audio.
	int outputMode = 1;  // 0=Gates 1=Amplitudes 2=Phases 3=Frequencies
	int rampMode = 1;    // 0=AD 1=Looping 2=AR
	int range = 1;       // 0=Control (LFO) 1=Audio

	dsp::BooleanTrigger modeButton;

	// Shift/level per output mode — same table as shiftForOutputMode() in the
	// firmware (firmwares/mod2-tides/tides.h), where the derivation is
	// documented. Paired with mixForOutputMode() below: the single OUT jack
	// emulates Tides' four outputs by tapping/mixing channels per mode.
	static float shiftForOutputMode(int mode) {
		switch (mode) {
			case 0:  return 1.0f;    // Gates (shift unused by the channel-1 tap)
			case 1:  return 0.598f;  // Amplitudes (channel 0 at full crossfade gain)
			case 2:  return 0.875f;  // Phases (quarter-phase spread)
			default: return 0.7f;    // Frequencies (ratio row 14 = C-E-G-C chord)
		}
	}

	// Single-jack multi-output emulation, same mapping as the firmware:
	//   Gates       channel 1 — the un-shaped ramp (raw saw / gate wave)
	//   Amplitudes  channel 0 — the classic shaped slope
	//   Phases      4 quarter-phase copies mixed — comb/chorus thickening
	//   Frequencies 4 ratio'd channels mixed — C-E-G-C chord stack
	static float mixForOutputMode(int mode, const tides::PolySlopeGenerator::OutputSample& s) {
		switch (mode) {
			case 0:  return s.channel[1];
			case 2:  return 0.4f * (s.channel[0] + s.channel[1] + s.channel[2] + s.channel[3]);
			case 3:  return 0.3f * (s.channel[0] + s.channel[1] + s.channel[2] + s.channel[3]);
			default: return s.channel[0];
		}
	}

	TinyTides() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(SHAPE_PARAM, 0.f, 1.f, 0.5f, "Shape");
		configParam(SLOPE_PARAM, 0.f, 1.f, 0.5f, "Slope");
		configParam(FREQ_PARAM, 0.f, 1.f, 0.5f, "Frequency", " Hz", 0.f, FREQ_RANGE_HZ, FREQ_MIN_HZ);
		configButton(MODE_PARAM, "Output mode (Gates/Amplitudes/Phases/Frequencies)");

		configInput(TRIG_INPUT, "Trigger / gate");
		configInput(SMOOTH_INPUT, "Smoothness CV (0-10 V)");
		configInput(CV_INPUT, "1V/Oct frequency");
		configOutput(AUDIO_OUTPUT, "OUT1 (channel 0)");

		generator.Init();
	}

	void onReset() override {
		generator.Init();
		previousGateFlags = stmlib::GATE_FLAG_LOW;
		blockPos = 0;
		outputMode = 1;
		rampMode = 1;
		range = 1;
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "outputMode", json_integer(outputMode));
		json_object_set_new(root, "rampMode", json_integer(rampMode));
		json_object_set_new(root, "range", json_integer(range));
		mod2WritePanelStyle(root, panelStyle);
		return root;
	}
	void dataFromJson(json_t* root) override {
		if (json_t* j = json_object_get(root, "outputMode"))
			outputMode = clamp((int) json_integer_value(j), 0, 3);
		if (json_t* j = json_object_get(root, "rampMode"))
			rampMode = clamp((int) json_integer_value(j), 0, 2);
		if (json_t* j = json_object_get(root, "range"))
			range = clamp((int) json_integer_value(j), 0, 1);
		mod2ReadPanelStyle(root, panelStyle);
	}

	void renderBlock(const ProcessArgs& args) {
		// Frequency: knob 20–2000 Hz, standard 1V/Oct on the Freq jack, then
		// normalized to cycles/sample (Render() clamps the top at 0.25).
		float freq = FREQ_MIN_HZ + FREQ_RANGE_HZ * params[FREQ_PARAM].getValue();
		freq *= std::pow(2.0f, inputs[CV_INPUT].getVoltage());
		if (range == 0)
			freq /= CONTROL_DIV;
		const float normalizedFreq = clamp(freq * args.sampleTime, 0.0001f, 0.25f);

		// Smoothness: 0–10 V CV -> 0..1, neutral 0.5 unpatched (hardware is a
		// gate mapped to 0.2 / 0.8).
		const float smoothness = inputs[SMOOTH_INPUT].isConnected()
			? clamp(inputs[SMOOTH_INPUT].getVoltage() / 10.f, 0.f, 1.f)
			: 0.5f;

		// Gate flags: level-sensitive like the firmware's digital trigger pin,
		// sampled once per block (~0.3 ms granularity).
		const bool gateHigh = inputs[TRIG_INPUT].getVoltage() >= 1.f;
		stmlib::GateFlags gateFlags[BLOCK];
		for (int i = 0; i < BLOCK; i++) {
			gateFlags[i] = stmlib::ExtractGateFlags(previousGateFlags, gateHigh);
			previousGateFlags = gateFlags[i];
		}

		generator.Render(
			static_cast<tides::RampMode>(rampMode),
			static_cast<tides::OutputMode>(outputMode),
			static_cast<tides::Range>(range),
			normalizedFreq,
			params[SLOPE_PARAM].getValue(),   // pw / slope
			params[SHAPE_PARAM].getValue(),   // shape
			smoothness,
			shiftForOutputMode(outputMode),
			gateFlags,
			nullptr,                          // no external ramp/clock input
			out,
			BLOCK);
	}

	void process(const ProcessArgs& args) override {
		// Mode button: cycle output mode (matches the firmware short press).
		if (modeButton.process(params[MODE_PARAM].getValue() > 0.5f))
			outputMode = (outputMode + 1) % 4;

		if (blockPos == 0)
			renderBlock(args);

		// Channels are normalized floats (roughly -1..+1) -> ±5 V, the same
		// full-scale conversion as the firmware's Clip16(x * 32768).
		const float sample = clamp(mixForOutputMode(outputMode, out[blockPos]), -1.f, 1.f);
		outputs[AUDIO_OUTPUT].setVoltage(sample * 5.f);
		lights[FEEDBACK_LIGHT].setBrightnessSmooth(std::fabs(sample), args.sampleTime);

		blockPos = (blockPos + 1) % BLOCK;
	}
};

struct TinyTidesWidget : ModuleWidget {
	TinyTidesWidget(TinyTides* module) {
		setModule(module);
		setMod2Panel(this, module, "res/mod2-tides.svg");
		// 4 HP Mod2 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<Reversed<RoundBlackKnob>>(mm2px(Vec(10.03f, 21.70f)), module, TinyTides::SHAPE_PARAM));
		addParam(createParamCentered<Reversed<RoundBlackKnob>>(mm2px(Vec(10.04f, 40.06f)), module, TinyTides::SLOPE_PARAM));
		addParam(createParamCentered<Reversed<RoundBlackKnob>>(mm2px(Vec(10.04f, 58.42f)), module, TinyTides::FREQ_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, TinyTides::MODE_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, TinyTides::FEEDBACK_LIGHT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, TinyTides::TRIG_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.30f)), module, TinyTides::SMOOTH_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, TinyTides::AUDIO_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, TinyTides::CV_INPUT));
	}

	void appendContextMenu(Menu* menu) override {
		TinyTides* m = dynamic_cast<TinyTides*>(module);
		if (!m)
			return;
		menu->addChild(new MenuSeparator);
		menu->addChild(createIndexPtrSubmenuItem("Output mode",
			{"Gates (raw ramp)", "Amplitudes (slope)", "Phases (4-phase stack)",
			 "Frequencies (chord)"}, &m->outputMode));
		menu->addChild(createIndexPtrSubmenuItem("Ramp mode",
			{"AD (envelope)", "Looping (cycle)", "AR (gate)"}, &m->rampMode));
		menu->addChild(createIndexPtrSubmenuItem("Range",
			{"Control (LFO)", "Audio"}, &m->range));
		appendMod2PanelMenu(menu, module);
	}
};

Model* modelTinyTides = createModel<TinyTides, TinyTidesWidget>("mod2-tides");
