#include "plugin.hpp"
#include <TardyCore.h>  // Shared Tardy delay engine (also used by the firmware)

/*
	Tardy — two-channel delay for triggers and gates.

	Port of firmwares/mod1-tardy/mod1-tardy.ino, itself a port of Sean Luke's
	GRAINS `tardy`. The ring buffers, the tick grid and the knob law all live in
	sc::TardyEngine; this file owns only the Rack I/O.

	Mod1 hardware mapping — 3 pots, 1 button, 1 LED, 4 flexible jacks:
		DLY A  -> channel A delay (POT1)
		DLY B  -> channel B delay (POT2), follows DLY A while LINK is on
		RANGE  -> full-scale delay: short ~107 ms / medium ~427 ms / long ~853 ms
		          (POT3, boundaries from the firmware's select3FromAdc)
		LINK   -> button; makes B follow A's knob, i.e. upstream's single
		          shared delay pot
		F1     -> trigger/gate input A
		F2     -> trigger/gate input B
		F3     -> delayed A (0/10 V gate)
		F4     -> delayed B (0/10 V gate)
		LED    -> lit while either output is high, dim at idle when LINK is on

	Divergences from the firmware (which lists its own divergences from GRAINS
	in its header — the 2-in/2-out reduction, RANGE, LINK and the bittest fix
	are all documented there and are shared, not Rack-only):
		- Input threshold is Rack's gate convention (1 V rising, 0.1 V falling)
		  rather than the hardware's ~2.5 V logic threshold, because Rack gates
		  are 0/10 V and the hardware's are 0/5 V.
		- LINK is an ordinary latching param, so the patch saves it; the
		  firmware needs EEPROM for the same job.
		- The delay knobs read out in milliseconds. That number depends on
		  RANGE, so it is computed from the engine rather than declared as a
		  fixed unit on the param.

	License:
	Apache License 2.0 for the engine: the original Tardy firmware is Copyright
	2023 Sean Luke (sean@cs.gmu.edu), from the GRAINS project
	(github.com/eclab/grains), Apache 2.0. Apache requires that notice to travel
	with the code and modified files to carry prominent notice of changes — it
	lives at firmwares/mod1-tardy/LICENSE.md, and the sketch header carries the
	list of changes.

	The Rack-side work in this file is MIT License, Copyright (c) 2026 Madelyn
	Yeary, see rack-plugins/LICENSE.md.
*/

struct Tardy : Module {
	enum ParamId {
		DELAY_A_PARAM,
		DELAY_B_PARAM,
		RANGE_PARAM,
		LINK_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		F1_INPUT,   // trigger/gate A
		F2_INPUT,   // trigger/gate B
		INPUTS_LEN
	};
	enum OutputId {
		F3_OUTPUT,  // delayed A
		F4_OUTPUT,  // delayed B
		OUTPUTS_LEN
	};
	enum LightId {
		ACTIVITY_LIGHT,
		LIGHTS_LEN
	};

	// All of the delay memory and timing lives in the shared engine.
	sc::TardyEngine tardy;

	// The engine wants the sustained gate LEVEL, not an edge, so these are read
	// back with isHigh() rather than from process()'s return value.
	dsp::SchmittTrigger gateA;
	dsp::SchmittTrigger gateB;

	Tardy();

	void onReset() override {
		tardy.reset();
	}

	void process(const ProcessArgs& args) override {
		tardy.setDelay01(0, params[DELAY_A_PARAM].getValue());
		tardy.setDelay01(1, params[DELAY_B_PARAM].getValue());
		tardy.setRange(sc::select3(params[RANGE_PARAM].getValue()));
		tardy.setLink(params[LINK_PARAM].getValue() > 0.5f);

		gateA.process(inputs[F1_INPUT].getVoltage(), 0.1f, 1.f);
		gateB.process(inputs[F2_INPUT].getVoltage(), 0.1f, 1.f);

		// The engine banks args.sampleTime and fires whole 2400 Hz ticks; the
		// outputs only move when one does. Triggers shorter than a tick are
		// OR-latched inside the engine, so nothing is dropped at audio rates.
		if (tardy.process(args.sampleTime, gateA.isHigh(), gateB.isHigh())) {
			outputs[F3_OUTPUT].setVoltage(tardy.out[0] ? 10.f : 0.f);
			outputs[F4_OUTPUT].setVoltage(tardy.out[1] ? 10.f : 0.f);
		}

		// LED mirrors the firmware: full while either output is high, dim at
		// idle when LINK is on so the toggle is visible at a glance.
		const bool active = tardy.out[0] || tardy.out[1];
		const float idle = (params[LINK_PARAM].getValue() > 0.5f) ? 0.08f : 0.f;
		lights[ACTIVITY_LIGHT].setBrightnessSmooth(active ? 1.f : idle, args.sampleTime);
	}
};

// The delay knobs are 0..1 controls whose real value depends on RANGE, so the
// tooltip asks the engine what the current setting actually works out to.
struct TardyDelayQuantity : ParamQuantity {
	int channel = 0;

	std::string getDisplayValueString() override {
		Tardy* m = dynamic_cast<Tardy*>(module);
		if (!m)
			return ParamQuantity::getDisplayValueString();
		return string::f("%.2f", m->tardy.delaySeconds(channel) * 1000.f);
	}
};

Tardy::Tardy() {
	config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

	TardyDelayQuantity* qa = configParam<TardyDelayQuantity>(
		DELAY_A_PARAM, 0.f, 1.f, 0.f, "Delay A", " ms");
	qa->channel = 0;
	TardyDelayQuantity* qb = configParam<TardyDelayQuantity>(
		DELAY_B_PARAM, 0.f, 1.f, 0.f, "Delay B", " ms");
	qb->channel = 1;

	// Continuous knob rather than a 3-way switch: the hardware control is a pot
	// read through select3FromAdc, and sc::select3 reproduces its boundaries.
	configParam(RANGE_PARAM, 0.f, 1.f, 1.f,
	            "Delay range (short 107 ms · medium 427 ms · long 853 ms)");
	configSwitch(LINK_PARAM, 0.f, 1.f, 0.f, "Link B to A's delay knob",
	             {"Off", "On"});

	configInput(F1_INPUT, "F1 trigger/gate A");
	configInput(F2_INPUT, "F2 trigger/gate B");

	configOutput(F3_OUTPUT, "F3 delayed A");
	configOutput(F4_OUTPUT, "F4 delayed B");
}

struct TardyWidget : ModuleWidget {
	TardyWidget(Tardy* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/mod1-tardy.svg")));
		// 4 HP Mod1/Mod2 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<Reversed<RoundBlackKnob>>(mm2px(Vec(10.03f, 21.7f)), module, Tardy::DELAY_A_PARAM));
		addParam(createParamCentered<Reversed<RoundBlackKnob>>(mm2px(Vec(10.04f, 40.06f)), module, Tardy::DELAY_B_PARAM));
		addParam(createParamCentered<Reversed<RoundBlackKnob>>(mm2px(Vec(10.04f, 58.42f)), module, Tardy::RANGE_PARAM));
		addParam(createParamCentered<VCVLatch>(mm2px(Vec(5.19f, 78.57f)), module, Tardy::LINK_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, Tardy::ACTIVITY_LIGHT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, Tardy::F1_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.3f)), module, Tardy::F2_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, Tardy::F3_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, Tardy::F4_OUTPUT));
	}
};

Model* modelTardy = createModel<Tardy, TardyWidget>("mod1-tardy");
