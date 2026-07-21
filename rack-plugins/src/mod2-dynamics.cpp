// WIP: Claude-generated maddie synths original (no upstream Hagiwo/Rob
// Scape firmware). Excluded from the VCV Rack build -- see WIP_SOURCES in
// the Makefile and the WIP block in plugin.cpp for how to re-enable it.
#include "plugin.hpp"
#include <DynamicsCore.h>  // Shared dynamics DSP (also used by mod2-dynamics firmware)

/*
	Dynamics — one-knob compressor + limiter (+ trigger ducker).

	Port of firmwares/mod2-dynamics/mod2-dynamics.ino (HAGIWO Mod2, RP2350).

	Mirrors the Mod2 hardware: 3 pots, 1 push button, 1 PWM LED, and the Mod2
	jack set (IN1, IN2, CV, OUT). For the Dynamics firmware:
		POT1 (A0) -> Amount (threshold + ratio + makeup swept together)
		POT2 (A1) -> Release (30 ms - 1 s; attack auto-scaled)
		POT3 (A2) -> unavailable on hardware (pin doubles as the audio input)
		BUTTON    -> mode: Compressor / Limiter / Ducker
		LED       -> gain-reduction meter (brighter = more GR)
		IN1       -> sidechain trigger (ducks the audio in Ducker mode)
		IN2       -> bypass gate (>1 V = dry)
		CV        -> AUDIO INPUT (sampled at the audio rate on hardware)
		OUT       -> audio output

	Deviations from hardware: the firmware's dry-blend and output-trim live on
	shift layers (hold BUTTON + turn POT1 / POT2) because POT3's pin is the
	audio input; here the physically-present-but-dead third knob becomes a
	proper parallel Mix control (the more musical of the two shift controls).
	Output trim is left at unity in Rack — the Amount curve already makes up
	the level, and Rack patches have headroom. The mode persists in the patch
	(firmware: flash).
*/

struct Dynamics : Module {
	enum ParamId {
		AMOUNT_PARAM,
		RELEASE_PARAM,
		MIX_PARAM,
		MODE_PARAM,   // momentary button — cycles Compressor / Limiter / Ducker
		PARAMS_LEN
	};
	enum InputId {
		DUCK_INPUT,   // IN1 — sidechain trigger (Ducker mode)
		BYPASS_INPUT, // IN2 — bypass gate (>1 V = dry)
		AUDIO_INPUT,  // CV jack — audio in
		INPUTS_LEN
	};
	enum OutputId {
		AUDIO_OUTPUT, // OUT
		OUTPUTS_LEN
	};
	enum LightId {
		GR_LIGHT,
		LIGHTS_LEN
	};

	// Dynamics state lives in the shared core (same DSP as the firmware).
	sc::DynamicsCore core;

	dsp::SchmittTrigger duckTrigger;
	dsp::BooleanTrigger modeButton;

	Dynamics() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(AMOUNT_PARAM, 0.f, 1.f, 0.3f, "Amount (glue → smash)", "%", 0.f, 100.f);
		configParam(RELEASE_PARAM, 0.f, 1.f, 0.4f, "Release (30 ms → 1 s)");
		configParam(MIX_PARAM, 0.f, 1.f, 0.f, "Parallel dry blend", "%", 0.f, 100.f);
		configButton(MODE_PARAM, "Mode (Compressor / Limiter / Ducker)");

		configInput(DUCK_INPUT, "IN1 sidechain trigger (ducks in Ducker mode)");
		configInput(BYPASS_INPUT, "IN2 bypass gate (>1 V = dry)");
		configInput(AUDIO_INPUT, "Audio");
		configOutput(AUDIO_OUTPUT, "Audio");

		configBypass(AUDIO_INPUT, AUDIO_OUTPUT);
		core.reset();
	}

	void onReset() override {
		core.reset();
		core.mode = sc::DYN_COMPRESSOR;
	}

	// The mode persists with the patch (the firmware stores it in flash).
	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "dynamicsMode", json_integer(core.mode));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* modeJ = json_object_get(rootJ, "dynamicsMode");
		if (modeJ)
			core.mode = (uint8_t)clamp((int)json_integer_value(modeJ), 0,
			                           sc::DYN_MODE_COUNT - 1);
	}

	void process(const ProcessArgs& args) override {
		if (modeButton.process(params[MODE_PARAM].getValue() > 0.5f))
			core.mode = (core.mode + 1) % sc::DYN_MODE_COUNT;

		// IN1 rising edge fires the ducker envelope (used in Ducker mode).
		if (duckTrigger.process(inputs[DUCK_INPUT].getVoltage(), 0.1f, 1.f))
			core.duckTrigger();

		// Same pot mappings as the firmware.
		core.amount = params[AMOUNT_PARAM].getValue();
		core.releaseSec = sc::dynamicsReleaseSec(params[RELEASE_PARAM].getValue());
		core.dryMix = params[MIX_PARAM].getValue();
		core.outTrim = 1.f;  // firmware's output-trim shift layer -> unity here

		// +/-5 V -> -1..1 through the shared core and back.
		const float in = inputs[AUDIO_INPUT].getVoltage() / 5.f;
		float out = core.process(in, args.sampleTime);

		// IN2 bypass gate: >1 V passes the dry input (firmware behaviour).
		if (inputs[BYPASS_INPUT].getVoltage() > 1.f)
			out = in;

		outputs[AUDIO_OUTPUT].setVoltage(clamp(out, -1.f, 1.f) * 5.f);

		// LED is a gain-reduction meter, as on hardware.
		lights[GR_LIGHT].setBrightness(core.ledLevel());
	}
};

struct DynamicsWidget : ModuleWidget {
	DynamicsWidget(Dynamics* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/mod2-dynamics.svg")));
		// 4 HP Mod1/Mod2 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.03f, 21.7f)), module, Dynamics::AMOUNT_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 40.06f)), module, Dynamics::RELEASE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 58.42f)), module, Dynamics::MIX_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, Dynamics::MODE_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, Dynamics::GR_LIGHT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, Dynamics::DUCK_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.3f)), module, Dynamics::BYPASS_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, Dynamics::AUDIO_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, Dynamics::AUDIO_INPUT));
	}
};

Model* modelDynamics = createModel<Dynamics, DynamicsWidget>("mod2-dynamics");
