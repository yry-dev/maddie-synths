#include "plugin.hpp"
#include <MotifCore.h>  // Shared Motif engine (also used by the firmware)

/*
	Motif — 16-step random melody generator, "Topograf for notes".

	Port of firmwares/mod1-motif/mod1-motif.ino (for Hagiwo Mod1), itself a port
	of Sean Luke's GRAINS `motif`. The melody lives in sc::MotifEngine; this file
	owns only the Rack I/O.

	Rather than rolling dice per step, Motif exposes a navigable space of short
	melodies: a base PATTERN of four 4-step groups each rising or falling, a
	fixed deviation phrase chosen by RANDOM, and a VARIANCE that scales how far
	that deviation pulls the melody off the pattern. The same three knob
	positions always give the same 16 notes.

		POT1  -> Variance (0..15, deviation strength)
		POT2  -> Random (0..31, which deviation phrase)
		POT3  -> Pattern (0..31: lower 16 minor, upper 16 major)
		BTN   -> Reset to step 1 (momentary; mirrors the F2 input)
		LED   -> Pitch height (brightness tracks the current note)
		F1    -> Clock input (advances one step per rising edge)
		F2    -> Reset input (returns to step 1 on a rising edge)
		F3    -> Pitch output (1V/oct, 0..3.417V over the 42-semitone range)
		F4    -> Trigger output (10V pulse, 10 ms, one per note)

	Divergences from the firmware: none in the melody — the engine is shared, so
	the same knob positions give the same notes on both. The two differences are
	I/O only. The firmware smooths its pots on a 4 ms tick to reproduce the
	original's Mozzi control rate, where Rack reads parameters directly (a knob
	nudged mid-step therefore takes effect on the very next clock here, and up to
	a few milliseconds later on hardware). And pitch leaves the hardware through
	a 10-bit PWM stage, roughly 6 cents of quantisation, where Rack emits the
	exact voltage.

	The transposition input, the five hand-measured GRAINS output calibration
	tables and the 8 KB PROGMEM deviation table are all gone; see the firmware
	header and MotifCore.h for why.

	License:
	Apache License 2.0 for the engine: the original Motif firmware is
	Copyright 2023 Sean Luke (sean@cs.gmu.edu), from the GRAINS project
	(github.com/eclab/grains). Apache requires its notice to travel with the
	code — it lives at firmwares/mod1-motif/LICENSE.md, and the firmware
	header's "Deliberate changes from the original" list is the notice of
	changes. Keep both.

	The Rack-side work in this file is MIT License, Copyright (c) 2026 Madelyn
	Yeary, see rack-plugins/LICENSE.md.
*/

struct Motif : Module {
	enum ParamId {
		VARIANCE_PARAM,
		RANDOM_PARAM,
		PATTERN_PARAM,
		RESET_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		CLOCK_INPUT,  // F1 — clock
		RESET_INPUT,  // F2 — reset
		INPUTS_LEN
	};
	enum OutputId {
		PITCH_OUTPUT,    // F3 — 1V/oct
		TRIGGER_OUTPUT,  // F4 — 10V, 10 ms
		OUTPUTS_LEN
	};
	enum LightId {
		PITCH_LIGHT,
		LIGHTS_LEN
	};

	sc::MotifEngine engine;
	dsp::SchmittTrigger clockTrig;
	dsp::SchmittTrigger resetInputTrig;
	dsp::BooleanTrigger resetParamTrig;
	dsp::PulseGenerator triggerPulse;

	Motif() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(VARIANCE_PARAM, 0.f, 1.f, 0.5f, "Variance");
		configParam(RANDOM_PARAM,   0.f, 1.f, 0.5f, "Random phrase");
		configParam(PATTERN_PARAM,  0.f, 1.f, 0.25f, "Pattern (minor <-> major)");
		configButton(RESET_PARAM, "Reset to step 1");
		configInput(CLOCK_INPUT, "Clock");
		configInput(RESET_INPUT, "Reset");
		configOutput(PITCH_OUTPUT,   "Pitch (1V/oct)");
		configOutput(TRIGGER_OUTPUT, "Trigger (10V, 10 ms)");
	}

	void onReset() override { engine.reset(); }

	void process(const ProcessArgs& args) override {
		const sc::MotifParams p = sc::motifMapParams(
			params[VARIANCE_PARAM].getValue(),
			params[RANDOM_PARAM].getValue(),
			params[PATTERN_PARAM].getValue());

		// Reset: rising edge on F2 or a front-panel button press. As on
		// hardware, this only rewinds the position — the starting note is
		// re-derived from the pattern on the next clock.
		if (resetInputTrig.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f))
			engine.resetPosition();
		if (resetParamTrig.process(params[RESET_PARAM].getValue() > 0.5f))
			engine.resetPosition();

		if (clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f)) {
			engine.step(p);
			triggerPulse.trigger(10e-3f);
		}

		outputs[PITCH_OUTPUT].setVoltage(sc::motifSemitoneToVolts(engine.semitone));
		outputs[TRIGGER_OUTPUT].setVoltage(triggerPulse.process(args.sampleTime) ? 10.f : 0.f);

		lights[PITCH_LIGHT].setBrightness((float)engine.semitone / (float)sc::kMotifSemitoneMax);
	}
};

struct MotifWidget : ModuleWidget {
	MotifWidget(Motif* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/mod1-motif.svg")));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		// 4 HP Mod1 panel — real hole centres (scripts/panels/tools/panel_map.py).
		// MOD1's three pots are reverse-wired, hence Reversed<>.
		addParam(createParamCentered<Reversed<RoundBlackKnob>>(mm2px(Vec(10.03f, 21.7f)), module, Motif::VARIANCE_PARAM));
		addParam(createParamCentered<Reversed<RoundBlackKnob>>(mm2px(Vec(10.04f, 40.06f)), module, Motif::RANDOM_PARAM));
		addParam(createParamCentered<Reversed<RoundBlackKnob>>(mm2px(Vec(10.04f, 58.42f)), module, Motif::PATTERN_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, Motif::RESET_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, Motif::PITCH_LIGHT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, Motif::CLOCK_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.3f)), module, Motif::RESET_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, Motif::PITCH_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, Motif::TRIGGER_OUTPUT));
	}
};

Model* modelMotif = createModel<Motif, MotifWidget>("mod1-motif");
