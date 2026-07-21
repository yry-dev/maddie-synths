// WIP: Claude-generated maddie synths original (no upstream Hagiwo/Rob
// Scape firmware). Excluded from the VCV Rack build -- see WIP_SOURCES in
// the Makefile and the WIP block in plugin.cpp for how to re-enable it.
#include "plugin.hpp"
#include <FreqShifterCore.h>  // Shared SSB DSP (also used by mod2-freq-shifter firmware)

/*
	Freq Shifter — Bode-style single-sideband frequency shifter.

	Port of firmwares/mod2-freq-shifter/mod2-freq-shifter.ino (HAGIWO Mod2, RP2350).

	Mirrors the Mod2 hardware: 3 pots, 1 push button, 1 PWM LED, and the Mod2
	jack set (IN1, IN2, CV, OUT). For the Freq Shifter firmware:
		POT1 (A0) -> Shift amount (bipolar, centre = 0 Hz; range set by button)
		POT2 (A1) -> Feedback (barberpole spiral)
		POT3 (A2) -> unavailable on hardware (pin doubles as the audio input)
		BUTTON    -> range: +/-20 Hz / +/-200 Hz / +/-1 kHz
		LED       -> rotates at the shift rate
		IN1       -> shift direction flip gate (>1 V = flip)
		IN2       -> bypass gate (>1 V = dry)
		CV        -> AUDIO INPUT (sampled at the audio rate on hardware)
		OUT       -> audio output

	Deviations from hardware: the firmware's wet/dry lives on a shift layer
	(hold BUTTON + turn POT1) because POT3's pin is the audio input; here the
	physically-present-but-dead third knob becomes a proper Mix control. The
	second shift layer (hold BUTTON + turn POT2 = up/down sideband blend) has
	no spare knob at all, so the sideband blend lives in the context menu
	(0 = up, 0.5 = both/ring-mod, 1 = down). The range persists in the patch
	(firmware: flash).
*/

struct FreqShifter : Module {
	enum ParamId {
		SHIFT_PARAM,
		FEEDBACK_PARAM,
		MIX_PARAM,
		MODE_PARAM,      // momentary button — cycles the range
		SIDEBAND_PARAM,  // context-menu slider (hardware: BUTTON + POT2)
		PARAMS_LEN
	};
	enum InputId {
		FLIP_INPUT,   // IN1 — shift direction flip gate
		BYPASS_INPUT, // IN2 — bypass gate (>1 V = dry)
		AUDIO_INPUT,  // CV jack — audio in
		INPUTS_LEN
	};
	enum OutputId {
		AUDIO_OUTPUT, // OUT
		OUTPUTS_LEN
	};
	enum LightId {
		ROTATE_LIGHT,
		LIGHTS_LEN
	};

	// Freq-shifter state lives in the shared core (same DSP as the firmware).
	sc::FreqShifterCore core;

	dsp::BooleanTrigger modeButton;

	FreqShifter() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(SHIFT_PARAM, 0.f, 1.f, 0.5f, "Shift amount (centre = 0 Hz)");
		configParam(FEEDBACK_PARAM, 0.f, 1.f, 0.f, "Feedback", "%", 0.f, 100.f);
		configParam(MIX_PARAM, 0.f, 1.f, 1.f, "Wet/dry mix", "%", 0.f, 100.f);
		configButton(MODE_PARAM, "Range (±20 Hz / ±200 Hz / ±1 kHz)");
		configParam(SIDEBAND_PARAM, 0.f, 1.f, 0.f, "Sideband (up ↔ ring-mod ↔ down)", "%", 0.f, 100.f);

		configInput(FLIP_INPUT, "IN1 shift direction flip gate (>1 V)");
		configInput(BYPASS_INPUT, "IN2 bypass gate (>1 V = dry)");
		configInput(AUDIO_INPUT, "Audio");
		configOutput(AUDIO_OUTPUT, "Audio");

		configBypass(AUDIO_INPUT, AUDIO_OUTPUT);

		// Load the Hilbert allpass coefficients now — onReset() only fires on
		// the user's Initialize action, not on module creation.
		onReset();
	}

	void onReset() override {
		core.reset();
		core.range = sc::FREQSHIFT_1000;
	}

	// The range persists with the patch (the firmware stores it in flash).
	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "range", json_integer(core.range));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* rangeJ = json_object_get(rootJ, "range");
		if (rangeJ)
			core.range = (uint8_t)clamp((int)json_integer_value(rangeJ), 0,
			                            sc::FREQSHIFT_RANGE_COUNT - 1);
	}

	void process(const ProcessArgs& args) override {
		if (modeButton.process(params[MODE_PARAM].getValue() > 0.5f))
			core.range = (core.range + 1) % sc::FREQSHIFT_RANGE_COUNT;

		// Same pot mappings as the firmware (shift depends on the range).
		const float rangeHz = sc::freqShifterRangeHz(core.range);
		core.shiftHz = sc::freqShifterShiftHz(params[SHIFT_PARAM].getValue(), rangeHz);
		core.feedback = sc::freqShifterFeedback(params[FEEDBACK_PARAM].getValue());
		core.sideband = params[SIDEBAND_PARAM].getValue();
		core.wet = params[MIX_PARAM].getValue();
		core.flip = inputs[FLIP_INPUT].getVoltage() > 1.f;

		// +/-5 V -> -1..1 through the shared core and back.
		const float in = inputs[AUDIO_INPUT].getVoltage() / 5.f;
		float out = core.process(in, args.sampleTime);

		// IN2 bypass gate: >1 V passes the dry input (firmware behaviour).
		if (inputs[BYPASS_INPUT].getVoltage() > 1.f)
			out = in;

		outputs[AUDIO_OUTPUT].setVoltage(clamp(out, -1.f, 1.f) * 5.f);

		// LED rotates at the shift rate, as on hardware.
		lights[ROTATE_LIGHT].setBrightness(core.ledLevel());
	}
};

struct FreqShifterWidget : ModuleWidget {
	FreqShifterWidget(FreqShifter* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/mod2-freq-shifter.svg")));
		// 4 HP Mod1/Mod2 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.03f, 21.7f)), module, FreqShifter::SHIFT_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 40.06f)), module, FreqShifter::FEEDBACK_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 58.42f)), module, FreqShifter::MIX_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, FreqShifter::MODE_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, FreqShifter::ROTATE_LIGHT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, FreqShifter::FLIP_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.3f)), module, FreqShifter::BYPASS_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, FreqShifter::AUDIO_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, FreqShifter::AUDIO_INPUT));
	}

	// The sideband blend is the firmware's second shift layer (BUTTON + POT2);
	// with no spare knob on the 4 HP panel it lives here in the context menu.
	void appendContextMenu(Menu* menu) override {
		FreqShifter* module = getModule<FreqShifter>();
		menu->addChild(new MenuSeparator);
		ui::Slider* slider = new ui::Slider;
		slider->quantity = module->paramQuantities[FreqShifter::SIDEBAND_PARAM];
		slider->box.size.x = 200.f;
		menu->addChild(slider);
	}
};

Model* modelFreqShifter = createModel<FreqShifter, FreqShifterWidget>("mod2-freq-shifter");
