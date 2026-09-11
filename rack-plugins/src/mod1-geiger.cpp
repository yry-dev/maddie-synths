#include "plugin.hpp"
#include <GeigerCore.h>  // Shared Geiger engine (also used by the mod1-geiger firmware)

/*
	Geiger — three-track random trigger generator and Bernoulli gate.

	Port of firmwares/mod1-geiger/mod1-geiger.ino, itself a port of Sean Luke's
	`geiger` firmware for the AE Modular GRAINS (github.com/eclab/grains).

	Every clock edge at F1 draws a coin per output: PROB1 is how often F2 fires,
	PROB2 how often F3 fires, PROB3 how often F4 fires. All three are independent,
	so any combination — including nothing at all — can happen on a given clock.
	Turn PROB3 past ~97.8% and the module switches to BERNOULLI mode, where
	exactly one output fires per clock: PROB1 is the chance it is F2, PROB2 splits
	the remainder between F3 and F4 (PROB1 = 1/3, PROB2 = 1/2 gives an even
	three-way split).

	Panel layout mirrors the Mod1 hardware exactly:
		PROB1 (POT1)  — firing probability for output 1
		PROB2 (POT2)  — firing probability for output 2
		PROB3 (POT3)  — firing probability for output 3; far right → BERNOULLI
		CLOCK (BTN)   — manual clock (momentary)
		LED           — lit while any output is high
		F1    IN      — clock / trigger input
		F2    OUT     — trigger 1
		F3    OUT     — trigger 2
		F4    OUT     — trigger 3

	Divergences from the firmware: none behavioural — the probability model, the
	Bernoulli nesting, the 10 ms trigger width and the PRNG all live in
	sc::GeigerEngine and are shared verbatim, so the same seed gives the same
	pattern on both. Two Rack-side notes:
	  - the seed comes from Rack's host RNG at construction and on Initialize
	    (the firmware mixes ADC noise off its spare CV pins), so two instances of
	    the module do not march in lockstep.
	  - the knob tooltips read plain 0..100%, but the engine follows upstream in
	    comparing against a 0..1023 scale with a 1000 divisor, so the top ~2.2% of
	    travel is a plateau where firing is guaranteed. That plateau is where
	    BERNOULLI lives on PROB3, which is why it is worth keeping rather than
	    normalising away.

	The divergences from GRAINS itself — no probability CV, a draw on every clock
	rather than every second one, a digital clock input, no skew on output 3 —
	are listed in the firmware header, which is the Apache "notice of changes".

	License:
	Apache License 2.0 for the engine: the original Geiger firmware is Copyright
	2023 Sean Luke (sean@cs.gmu.edu), from the GRAINS project
	(github.com/eclab/grains). Apache requires the license notice to travel with
	the code and modified files to carry prominent notice of changes; the notice
	ships as firmwares/mod1-geiger/LICENSE.md and the change list lives in the
	firmware header. Unlike the CC0 HAGIWO modules in this plugin, this one
	carries that condition — keep the notice.

	The Rack-side work in this file is MIT License, Copyright (c) 2026 Madelyn
	Yeary, see rack-plugins/LICENSE.md.
*/

struct Geiger : Module {
	enum ParamId {
		PROB1_PARAM,
		PROB2_PARAM,
		PROB3_PARAM,
		MANUAL_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		F1_INPUT,    // clock / trigger in
		INPUTS_LEN
	};
	enum OutputId {
		F2_OUTPUT,   // trigger 1
		F3_OUTPUT,   // trigger 2
		F4_OUTPUT,   // trigger 3
		OUTPUTS_LEN
	};
	enum LightId {
		ACTIVITY_LIGHT,
		LIGHTS_LEN
	};

	sc::GeigerEngine geiger;

	dsp::SchmittTrigger clockIn;
	dsp::BooleanTrigger manualBtn;

	Geiger() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(PROB1_PARAM, 0.f, 1.f, 0.5f, "Output 1 probability", "%", 0.f, 100.f);
		configParam(PROB2_PARAM, 0.f, 1.f, 0.5f, "Output 2 probability", "%", 0.f, 100.f);
		configParam(PROB3_PARAM, 0.f, 1.f, 0.5f, "Output 3 probability / Bernoulli", "%", 0.f, 100.f);
		configButton(MANUAL_PARAM, "Manual clock");

		configInput(F1_INPUT, "F1 clock");
		configOutput(F2_OUTPUT, "F2 trigger 1");
		configOutput(F3_OUTPUT, "F3 trigger 2");
		configOutput(F4_OUTPUT, "F4 trigger 3");

		// Independent streams per instance; the firmware gets the same effect
		// from ADC noise on its spare CV pins.
		geiger.seed(random::u32());
	}

	void onReset() override {
		geiger.reset();
		geiger.seed(random::u32());
	}

	void process(const ProcessArgs& args) override {
		sc::GeigerParams p = sc::geigerMapParams(
			params[PROB1_PARAM].getValue(),
			params[PROB2_PARAM].getValue(),
			params[PROB3_PARAM].getValue()
		);

		bool clockRose = clockIn.process(inputs[F1_INPUT].getVoltage(), 0.1f, 1.f);
		bool btnPress  = manualBtn.process(params[MANUAL_PARAM].getValue() > 0.5f);

		sc::GeigerResult r = geiger.process(args.sampleTime, clockRose || btnPress, p);

		outputs[F2_OUTPUT].setVoltage(r.out1 ? 10.f : 0.f);
		outputs[F3_OUTPUT].setVoltage(r.out2 ? 10.f : 0.f);
		outputs[F4_OUTPUT].setVoltage(r.out3 ? 10.f : 0.f);
		// 10 ms triggers are on the edge of visible, so let the light decay.
		lights[ACTIVITY_LIGHT].setBrightnessSmooth(r.any ? 1.f : 0.f, args.sampleTime);
	}
};

struct GeigerWidget : ModuleWidget {
	GeigerWidget(Geiger* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/mod1-geiger.svg")));
		// 4 HP Mod1 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		// Reversed<> because the Mod1 pots are wired backwards on the panel.
		addParam(createParamCentered<Reversed<RoundBlackKnob>>(mm2px(Vec(10.03f, 21.7f)), module, Geiger::PROB1_PARAM));
		addParam(createParamCentered<Reversed<RoundBlackKnob>>(mm2px(Vec(10.04f, 40.06f)), module, Geiger::PROB2_PARAM));
		addParam(createParamCentered<Reversed<RoundBlackKnob>>(mm2px(Vec(10.04f, 58.42f)), module, Geiger::PROB3_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, Geiger::MANUAL_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, Geiger::ACTIVITY_LIGHT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, Geiger::F1_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.3f)), module, Geiger::F2_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, Geiger::F3_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, Geiger::F4_OUTPUT));
	}
};

Model* modelGeiger = createModel<Geiger, GeigerWidget>("mod1-geiger");
