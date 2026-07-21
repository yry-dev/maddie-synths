// WIP: Claude-generated maddie synths original (no upstream Hagiwo/Rob
// Scape firmware). Excluded from the VCV Rack build -- see WIP_SOURCES in
// the Makefile and the WIP block in plugin.cpp for how to re-enable it.
#include "plugin.hpp"
#include <FilterCore.h>  // Shared filter DSP (also used by mod2-filter firmware)

/*
	Filter — tilt EQ + resonant cleanup filters.

	Port of firmwares/mod2-filter/mod2-filter.ino (HAGIWO Mod2, RP2350).

	Mirrors the Mod2 hardware: 3 pots, 1 push button, 1 PWM LED, and the Mod2
	jack set (IN1, IN2, CV, OUT). For the Filter firmware:
		POT1 (A0) -> cutoff / tilt pivot (20 Hz - 16 kHz, exponential)
		POT2 (A1) -> resonance (LP/HP/BP) or tilt amount (EQ, bipolar +/-6 dB)
		POT3 (A2) -> unavailable on hardware (pin doubles as the audio input)
		BUTTON    -> mode: Tilt EQ / Low-pass / High-pass / Band-pass
		LED       -> follows the output level
		IN1       -> (spare on hardware; no port here)
		IN2       -> bypass gate (>1 V = dry)
		CV        -> AUDIO INPUT (sampled at the audio rate on hardware)
		OUT       -> audio output

	Deviations from hardware: the firmware's wet/dry lives on a shift layer
	(hold BUTTON + turn POT1) because POT3's pin is the audio input; here the
	physically-present-but-dead third knob becomes a proper Mix control. The
	firmware's second shift layer (BUTTON + POT2 = output trim) has no free knob
	in Rack, so trim is fixed at unity here (use a downstream VCA). The mode
	persists in the patch (firmware: flash).
*/

struct Filter : Module {
	enum ParamId {
		FREQ_PARAM,
		SHAPE_PARAM,  // resonance (LP/HP/BP) or tilt amount (EQ)
		MIX_PARAM,
		MODE_PARAM,   // momentary button — cycles Tilt / LP / HP / BP
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
		LEVEL_LIGHT,
		LIGHTS_LEN
	};

	// Filter state lives in the shared core (same DSP as the firmware).
	sc::FilterCore core;

	dsp::BooleanTrigger modeButton;

	Filter() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(FREQ_PARAM, 0.f, 1.f, 0.5f, "Cutoff / pivot (20 Hz → 16 kHz)");
		configParam(SHAPE_PARAM, 0.f, 1.f, 0.5f, "Resonance / tilt (flat at noon)");
		configParam(MIX_PARAM, 0.f, 1.f, 1.f, "Wet/dry mix", "%", 0.f, 100.f);
		configButton(MODE_PARAM, "Mode (Tilt EQ / LP / HP / BP)");

		configInput(BYPASS_INPUT, "IN2 bypass gate (>1 V = dry)");
		configInput(AUDIO_INPUT, "Audio");
		configOutput(AUDIO_OUTPUT, "Audio");

		configBypass(AUDIO_INPUT, AUDIO_OUTPUT);
		core.reset();
	}

	void onReset() override {
		core.reset();
		core.mode = sc::FILTER_TILT;
	}

	// The mode persists with the patch (the firmware stores it in flash).
	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "filterMode", json_integer(core.mode));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* modeJ = json_object_get(rootJ, "filterMode");
		if (modeJ)
			core.mode = (uint8_t)clamp((int)json_integer_value(modeJ), 0,
			                           sc::FILTER_MODE_COUNT - 1);
	}

	void process(const ProcessArgs& args) override {
		if (modeButton.process(params[MODE_PARAM].getValue() > 0.5f))
			core.mode = (core.mode + 1) % sc::FILTER_MODE_COUNT;

		// Same pot mappings as the firmware (the core does the unit mapping).
		core.cutoffPot = params[FREQ_PARAM].getValue();
		core.shapePot = params[SHAPE_PARAM].getValue();
		core.wet = params[MIX_PARAM].getValue();
		core.trim = 1.f;  // firmware's POT2 shift-layer trim has no knob here

		// +/-5 V -> -1..1 through the shared core and back.
		const float in = inputs[AUDIO_INPUT].getVoltage() / 5.f;
		float out = core.process(in, args.sampleTime);

		// IN2 bypass gate: >1 V passes the dry input (firmware behaviour).
		if (inputs[BYPASS_INPUT].getVoltage() > 1.f)
			out = in;

		outputs[AUDIO_OUTPUT].setVoltage(clamp(out, -1.f, 1.f) * 5.f);

		// LED follows the output level, as on hardware.
		lights[LEVEL_LIGHT].setBrightness(core.ledLevel());
	}
};

struct FilterWidget : ModuleWidget {
	FilterWidget(Filter* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/mod2-filter.svg")));
		// 4 HP Mod1/Mod2 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.03f, 21.7f)), module, Filter::FREQ_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 40.06f)), module, Filter::SHAPE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 58.42f)), module, Filter::MIX_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, Filter::MODE_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, Filter::LEVEL_LIGHT));

		// IN1 is spare on this firmware, so its jack position stays empty.
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.3f)), module, Filter::BYPASS_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, Filter::AUDIO_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, Filter::AUDIO_INPUT));
	}
};

Model* modelFilter = createModel<Filter, FilterWidget>("mod2-filter");
