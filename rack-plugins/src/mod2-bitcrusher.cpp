// WIP: Claude-generated maddie synths original (no upstream Hagiwo/Rob
// Scape firmware). Excluded from the VCV Rack build -- see WIP_SOURCES in
// the Makefile and the WIP block in plugin.cpp for how to re-enable it.
#include "plugin.hpp"
#include <BitcrusherCore.h>  // Shared crush DSP (also used by mod2-bitcrusher firmware)

/*
	Bitcrusher — bit-depth & sample-rate reduction effect.

	Port of firmwares/mod2-bitcrusher/mod2-bitcrusher.ino (HAGIWO Mod2, RP2350).

	Mirrors the Mod2 hardware: 3 pots, 1 push button, 1 PWM LED, and the Mod2
	jack set (IN1, IN2, CV, OUT). For the Bitcrusher firmware:
		POT1 (A0) -> Crush rate (full rate -> ~200 Hz, exponential taper)
		POT2 (A1) -> Bit depth (16 -> 1 bits, continuous)
		POT3 (A2) -> unavailable on hardware (pin doubles as the audio input)
		BUTTON    -> quantizer style: truncate / TPDF dither / AND-mask
		LED       -> crushed output level
		IN1       -> external crush clock (overrides the rate knob)
		IN2       -> bypass gate (>1 V = dry)
		CV        -> AUDIO INPUT (sampled at the audio rate on hardware)
		OUT       -> audio output

	Deviations from hardware: the firmware's wet/dry lives on a shift layer
	(hold BUTTON + turn POT1) because POT3's pin is the audio input; here the
	physically-present-but-dead third knob becomes a proper Mix control. The
	rate taper tops out at Rack's own sample rate instead of the hardware's
	fixed ~36.6 kHz, so the knob at zero is fully transparent at any engine
	rate. The quantizer style persists in the patch (firmware: flash).
*/

struct Bitcrusher : Module {
	enum ParamId {
		RATE_PARAM,
		BITS_PARAM,
		MIX_PARAM,
		MODE_PARAM,   // momentary button — cycles quantizer style
		PARAMS_LEN
	};
	enum InputId {
		CLOCK_INPUT,  // IN1 — external crush clock
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

	// Crush state lives in the shared core (same DSP as the firmware).
	sc::BitcrusherCore core;

	dsp::SchmittTrigger clockTrigger;
	dsp::BooleanTrigger modeButton;

	Bitcrusher() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(RATE_PARAM, 0.f, 1.f, 0.f, "Crush rate (full rate → 200 Hz)");
		configParam(BITS_PARAM, 0.f, 1.f, 0.f, "Bit depth (16 → 1 bits)");
		configParam(MIX_PARAM, 0.f, 1.f, 1.f, "Wet/dry mix", "%", 0.f, 100.f);
		configButton(MODE_PARAM, "Quantizer style (truncate / dither / AND-mask)");

		configInput(CLOCK_INPUT, "IN1 external crush clock");
		configInput(BYPASS_INPUT, "IN2 bypass gate (>1 V = dry)");
		configInput(AUDIO_INPUT, "Audio");
		configOutput(AUDIO_OUTPUT, "Audio");

		configBypass(AUDIO_INPUT, AUDIO_OUTPUT);
	}

	void onReset() override {
		core.reset();
		core.mode = sc::BITCRUSH_TRUNCATE;
	}

	// Quantizer style persists with the patch (the firmware stores it in flash).
	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "quantMode", json_integer(core.mode));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* modeJ = json_object_get(rootJ, "quantMode");
		if (modeJ)
			core.mode = (uint8_t)clamp((int)json_integer_value(modeJ), 0,
			                           sc::BITCRUSH_MODE_COUNT - 1);
	}

	void process(const ProcessArgs& args) override {
		if (modeButton.process(params[MODE_PARAM].getValue() > 0.5f))
			core.mode = (core.mode + 1) % sc::BITCRUSH_MODE_COUNT;

		// Same pot mappings as the firmware; the rate taper spans Rack's own
		// sample rate down to 200 Hz.
		core.rateHz = sc::bitcrusherRateHz(params[RATE_PARAM].getValue(), args.sampleRate);
		core.bits = sc::bitcrusherBits(params[BITS_PARAM].getValue());
		core.wet = params[MIX_PARAM].getValue();

		// IN1 patched = external crush clock (sample-and-hold captures on the
		// rising edge only — audio-rate FM of the crush, as on hardware).
		const bool useExt = inputs[CLOCK_INPUT].isConnected();
		const bool tick = clockTrigger.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f);

		// +/-5 V -> -1..1 through the shared core and back.
		const float in = inputs[AUDIO_INPUT].getVoltage() / 5.f;
		float out = core.process(in, args.sampleTime, useExt, tick);

		// IN2 bypass gate: >1 V passes the dry input (firmware behaviour).
		if (inputs[BYPASS_INPUT].getVoltage() > 1.f)
			out = in;

		outputs[AUDIO_OUTPUT].setVoltage(clamp(out, -1.f, 1.f) * 5.f);

		// LED follows the crushed output level, as on hardware.
		lights[OUT_LIGHT].setBrightnessSmooth(std::fabs(out), args.sampleTime);
	}
};

struct BitcrusherWidget : ModuleWidget {
	BitcrusherWidget(Bitcrusher* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/mod2-bitcrusher.svg")));
		// 4 HP Mod1/Mod2 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.03f, 21.7f)), module, Bitcrusher::RATE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 40.06f)), module, Bitcrusher::BITS_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 58.42f)), module, Bitcrusher::MIX_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, Bitcrusher::MODE_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, Bitcrusher::OUT_LIGHT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, Bitcrusher::CLOCK_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.3f)), module, Bitcrusher::BYPASS_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, Bitcrusher::AUDIO_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, Bitcrusher::AUDIO_INPUT));
	}
};

Model* modelBitcrusher = createModel<Bitcrusher, BitcrusherWidget>("mod2-bitcrusher");
