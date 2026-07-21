// WIP: Claude-generated maddie synths original (no upstream Hagiwo/Rob
// Scape firmware). Excluded from the VCV Rack build -- see WIP_SOURCES in
// the Makefile and the WIP block in plugin.cpp for how to re-enable it.
#include "plugin.hpp"
#include <TremoloCore.h>  // Shared tremolo DSP (also used by mod2-tremolo firmware)

/*
	Tremolo — LFO-driven VCA (amplitude modulation), clock-syncable.

	Port of firmwares/mod2-tremolo/mod2-tremolo.ino (HAGIWO Mod2, RP2350).

	Mirrors the Mod2 hardware: 3 pots, 1 push button, 1 PWM LED, and the Mod2
	jack set (IN1, IN2, CV, OUT). For the Tremolo firmware:
		POT1 (A0) -> LFO rate (0.1 - 30 Hz) / clock division when clocked
		POT2 (A1) -> Depth (0 - 100%, top = full chop)
		POT3 (A2) -> unavailable on hardware (pin doubles as the audio input)
		BUTTON    -> LFO shape: Sine / Triangle / Square / Ramp-down
		LED       -> breathes with the LFO
		IN1       -> clock in (rate locks to musical divisions of the period)
		IN2       -> LFO phase reset (chop syncs to the downbeat)
		CV        -> AUDIO INPUT (sampled at the audio rate on hardware)
		OUT       -> audio output

	Deviations from hardware: the firmware's wet/dry lives on a shift layer
	(hold BUTTON + turn POT1) because POT3's pin is the audio input; here the
	physically-present-but-dead third knob becomes a proper Mix control, and
	the button's long-press tap-tempo is dropped (patch a clock into IN1
	instead). The LFO shape persists in the patch (firmware: flash).
*/

struct Tremolo : Module {
	enum ParamId {
		RATE_PARAM,
		DEPTH_PARAM,
		MIX_PARAM,
		SHAPE_PARAM,   // momentary button — cycles Sine / Triangle / Square / Ramp
		PARAMS_LEN
	};
	enum InputId {
		CLOCK_INPUT,  // IN1 — clock sync
		RESET_INPUT,  // IN2 — LFO phase reset
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

	// Tremolo state lives in the shared core (same DSP as the firmware).
	sc::TremoloCore core;

	dsp::SchmittTrigger clockTrigger;
	dsp::SchmittTrigger resetTrigger;
	dsp::BooleanTrigger shapeButton;
	float clockTimer = 0.f;   // seconds since the last IN1 edge
	float clockPeriod = 0.f;  // measured IN1 period (0 = none yet)

	static constexpr float MIN_RATE_HZ = 0.1f;
	static constexpr float MAX_RATE_HZ = 30.f;

	Tremolo() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(RATE_PARAM, 0.f, 1.f, 0.5f, "LFO rate (0.1 → 30 Hz; clocked: division)");
		configParam(DEPTH_PARAM, 0.f, 1.f, 0.5f, "Depth", "%", 0.f, 100.f);
		configParam(MIX_PARAM, 0.f, 1.f, 1.f, "Wet/dry mix", "%", 0.f, 100.f);
		configButton(SHAPE_PARAM, "Shape (Sine / Triangle / Square / Ramp)");

		configInput(CLOCK_INPUT, "IN1 clock (locks rate to musical divisions)");
		configInput(RESET_INPUT, "IN2 LFO phase reset");
		configInput(AUDIO_INPUT, "Audio");
		configOutput(AUDIO_OUTPUT, "Audio");

		configBypass(AUDIO_INPUT, AUDIO_OUTPUT);
		core.reset();
	}

	void onReset() override {
		core.reset();
		core.shape = sc::TREMOLO_SINE;
	}

	// The LFO shape persists with the patch (the firmware stores it in flash).
	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "shape", json_integer(core.shape));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* shapeJ = json_object_get(rootJ, "shape");
		if (shapeJ)
			core.shape = (uint8_t)clamp((int)json_integer_value(shapeJ), 0,
			                            sc::TREMOLO_SHAPE_COUNT - 1);
	}

	void process(const ProcessArgs& args) override {
		if (shapeButton.process(params[SHAPE_PARAM].getValue() > 0.5f))
			core.shape = (core.shape + 1) % sc::TREMOLO_SHAPE_COUNT;

		// IN2 patched = LFO phase reset on the rising edge (downbeat sync).
		if (resetTrigger.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f))
			core.resetPhase();

		// IN1 patched = clock sync: the rate knob picks a musical ratio of the
		// measured clock rate, exactly as on hardware.
		if (inputs[CLOCK_INPUT].isConnected()) {
			clockTimer += args.sampleTime;
			if (clockTrigger.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f)) {
				if (clockTimer > 0.f && clockTimer < 1.f / MIN_RATE_HZ)
					clockPeriod = clockTimer;
				clockTimer = 0.f;
			}
		}
		else {
			clockTimer = 0.f;
			clockPeriod = 0.f;
		}

		const float rateKnob = params[RATE_PARAM].getValue();
		if (clockPeriod > 0.f)
			core.rateHz = clamp((1.f / clockPeriod) * sc::tremoloClockRatio(rateKnob),
			                    MIN_RATE_HZ, MAX_RATE_HZ);
		else
			core.rateHz = sc::tremoloRateHz(rateKnob);

		core.depth = sc::tremoloDepth(params[DEPTH_PARAM].getValue());
		core.wet = params[MIX_PARAM].getValue();

		// +/-5 V -> -1..1 through the shared core and back.
		const float in = inputs[AUDIO_INPUT].getVoltage() / 5.f;
		const float out = core.process(in, args.sampleTime);
		outputs[AUDIO_OUTPUT].setVoltage(clamp(out, -1.f, 1.f) * 5.f);

		// LED breathes with the LFO, as on hardware.
		lights[LFO_LIGHT].setBrightness(core.ledLevel());
	}
};

struct TremoloWidget : ModuleWidget {
	TremoloWidget(Tremolo* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/mod2-tremolo.svg")));
		// 4 HP Mod1/Mod2 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.03f, 21.7f)), module, Tremolo::RATE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 40.06f)), module, Tremolo::DEPTH_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 58.42f)), module, Tremolo::MIX_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, Tremolo::SHAPE_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, Tremolo::LFO_LIGHT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, Tremolo::CLOCK_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.3f)), module, Tremolo::RESET_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, Tremolo::AUDIO_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, Tremolo::AUDIO_INPUT));
	}
};

Model* modelTremolo = createModel<Tremolo, TremoloWidget>("mod2-tremolo");
