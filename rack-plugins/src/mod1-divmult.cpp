#include "plugin.hpp"
#include <DivMultCore.h>  // Shared divider/multiplier engine (also used by the firmware)

/*
	Div Mult — two-track clock divider AND multiplier on one ratio dial.

	Port of firmwares/mod1-divmult/mod1-divmult.ino, itself a combined port of
	TWO of Sean Luke's GRAINS firmwares: `divvy` (two-track clock divider) and
	`multiple` (two-track clock multiplier), both from github.com/eclab/grains
	under the Apache License 2.0. Fusing them is the headline divergence — they
	are the same instrument from opposite sides, so their option tables are
	concatenated into one 21-position dial with a new x1 in the join:

		x16 x7 x6 x5 x4 x3 swing2 x2 | x1 | /2 /2·1 /3 /4 /4·2 /6 /8 /16 /24 /32 /64 /96

	`multiple`'s table runs from the left edge to the middle in its own order,
	`divvy`'s from the middle to the right edge in its own order; neither is
	reordered or trimmed. Each track picks anywhere in the range, so this one
	module covers all of divvy, all of multiple, and the combination neither
	upstream could reach — a divided and a multiplied track off one clock.

	Everything above the I/O is sc::DivMultEngine, shared verbatim with the
	firmware: both option tables, the counted-edge divider, the measured-period
	multiplier, the swing-2 suppression and both pulse-width rules. This file
	owns only Rack I/O.

	Timing translation: the firmware steps the engine once per main loop with a
	dt taken from micros(); here it steps once per sample with args.sampleTime.
	Divisions count clock edges and so are identical either way; multiplications
	work in seconds and so are identical too — which is the point of moving them
	off upstream's loop-iteration counter.

	Panel layout mirrors Mod1 hardware:
		RATIO A knob -> track A ratio, 21 positions (A0)
		RATIO B knob -> track B ratio, same range, independent (A1)
		WIDTH knob   -> pulse width, shared by all three outputs (A2)
		RESET button -> restart all three tracks (BTN; GRAINS used a reset jack,
		                MOD1 has no spare input, so the button inherits the job)
		F1 (IN)      -> clock
		F2 (OUT)     -> track A, 0/10 V
		F3 (OUT)     -> track B, 0/10 V
		F4 (OUT)     -> clock thru at x1 with the shared pulse width — the third
		                output MOD1 has room for and GRAINS did not
		LED          -> output activity: full for A, dim for B, dimmest for the
		                thru, which fires on every clock and would otherwise wash
		                the other two out

	The ratio knobs are continuous 0..1 controls fed to the engine rather than
	21-position switches, because the hardware control is a pot read as
	`(adc * 21) >> 10` and the shared mapper reproduces its boundaries exactly.
	The tooltip names the selected ratio so the dial is still readable. The
	selector carries a fifth-of-a-slot hysteresis, which on hardware stops a pot
	parked on a boundary from dithering between two ratios (see the sketch
	header — both upstreams misbehave there, in opposite directions); in Rack a
	knob does not dither, so it only shows up as the knob having to cross a
	little way past a boundary before the ratio changes.

	Pulse width is not the same thing on both sides of the dial, and that is
	upstream's design, not an artifact: a divided output rounds the pot down to
	whole clocks (and sends a 10 ms trigger at zero), while a multiplied output
	takes the pot as a fraction of its own sub-beat. The full list of deliberate
	divergences from the two originals is in the sketch header.

	License:
	Apache License 2.0, Copyright 2023 Sean Luke (sean@cs.gmu.edu) — this module
	and its sc::DivMultEngine are derived from the GRAINS `divvy` and `multiple`
	firmwares, and Apache 2.0 permits modification and redistribution (including
	commercially) only so long as the attribution and license notice ships with
	every copy or substantial portion of the work, and modified files carry
	prominent notice of change. The notice is kept at
	firmwares/mod1-divmult/LICENSE.md; keep it there and ship it. Unlike the CC0
	HAGIWO modules in this plugin, this one carries conditions.
	The Rack-side work in this file is MIT License, Copyright (c) 2026 Madelyn
	Yeary, see rack-plugins/LICENSE.md.
*/

struct DivMult : Module {
	enum ParamId {
		RATIO_A_PARAM,
		RATIO_B_PARAM,
		WIDTH_PARAM,
		RESET_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		F1_CLOCK_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		F2_A_OUTPUT,
		F3_B_OUTPUT,
		F4_THRU_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		ACTIVITY_LIGHT,
		LIGHTS_LEN
	};

	sc::DivMultEngine engine;
	dsp::SchmittTrigger clockTrigger;
	dsp::BooleanTrigger resetBtnTrigger;

	DivMult();

	void onReset() override { engine.reset(); }

	void process(const ProcessArgs& args) override {
		if (resetBtnTrigger.process(params[RESET_PARAM].getValue() > 0.5f))
			engine.reset();

		// Ratios and width, mapped by the same helpers the firmware uses. The
		// engine applies the selector hysteresis and debounces a ratio change
		// itself (upstream's OPTION_WAIT).
		engine.setRatioPot(0, params[RATIO_A_PARAM].getValue());
		engine.setRatioPot(1, params[RATIO_B_PARAM].getValue());
		engine.setOption(2, sc::kDivMultUnity);  // F4 is always the x1 thru
		engine.setPulseWidth(params[WIDTH_PARAM].getValue());

		const bool clockRose = clockTrigger.process(inputs[F1_CLOCK_INPUT].getVoltage(), 0.1f, 1.f);
		engine.step(args.sampleTime, clockRose);

		const bool a    = engine.out(0);
		const bool b    = engine.out(1);
		const bool thru = engine.out(2);

		outputs[F2_A_OUTPUT].setVoltage(a ? 10.f : 0.f);
		outputs[F3_B_OUTPUT].setVoltage(b ? 10.f : 0.f);
		outputs[F4_THRU_OUTPUT].setVoltage(thru ? 10.f : 0.f);

		const float led = a ? 1.f : (b ? 0.4f : (thru ? 0.1f : 0.f));
		lights[ACTIVITY_LIGHT].setBrightnessSmooth(led, args.sampleTime);
	}
};

// The ratio knobs are 0..1 pots whose meaning is a table index, so the tooltip
// asks the engine which ratio is actually selected rather than re-deriving it
// from the knob angle — the selector has hysteresis, so within a fifth of a
// slot either side of a boundary those two answers differ, and the one worth
// showing is the one that is playing.
struct DivMultRatioQuantity : ParamQuantity {
	int track = 0;

	std::string getDisplayValueString() override {
		DivMult* m = dynamic_cast<DivMult*>(module);
		if (!m)
			return sc::divMultRatioName(sc::divMultOptionFromPot(getValue()));
		return sc::divMultRatioName(m->engine.tracks[track].option);
	}
};

DivMult::DivMult() {
	config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

	// Default both tracks to x1 — the middle of the dial, and the one ratio
	// that is audible before you have decided what you want.
	const float unity = ((float)sc::kDivMultUnity + 0.5f) / (float)sc::kDivMultOptions;
	configParam<DivMultRatioQuantity>(RATIO_A_PARAM, 0.f, 1.f, unity, "Track A ratio")->track = 0;
	configParam<DivMultRatioQuantity>(RATIO_B_PARAM, 0.f, 1.f, unity, "Track B ratio")->track = 1;
	configParam(WIDTH_PARAM, 0.f, 1.f, 0.5f, "Pulse width", "%", 0.f, 100.f);
	configButton(RESET_PARAM, "Reset both tracks");

	configInput(F1_CLOCK_INPUT, "F1 clock");
	configOutput(F2_A_OUTPUT,    "F2 track A");
	configOutput(F3_B_OUTPUT,    "F3 track B");
	configOutput(F4_THRU_OUTPUT, "F4 clock thru (x1)");
}

struct DivMultWidget : ModuleWidget {
	DivMultWidget(DivMult* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/mod1-divmult.svg")));

		// 4 HP Mod1 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		// Reversed<> because the Mod1 pots are wired backwards on the panel.
		addParam(createParamCentered<Reversed<RoundBlackKnob>>(mm2px(Vec(10.03f, 21.7f)), module, DivMult::RATIO_A_PARAM));
		addParam(createParamCentered<Reversed<RoundBlackKnob>>(mm2px(Vec(10.04f, 40.06f)), module, DivMult::RATIO_B_PARAM));
		addParam(createParamCentered<Reversed<RoundBlackKnob>>(mm2px(Vec(10.04f, 58.42f)), module, DivMult::WIDTH_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, DivMult::RESET_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, DivMult::ACTIVITY_LIGHT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, DivMult::F1_CLOCK_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.3f)), module, DivMult::F2_A_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, DivMult::F3_B_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, DivMult::F4_THRU_OUTPUT));
	}
};

Model* modelDivMult = createModel<DivMult, DivMultWidget>("mod1-divmult");
