#include "plugin.hpp"
#include <SwitchbladeCore.h>  // Shared Switchblade engine (also used by the firmware)

/*
	Switchblade — summing attenuverter with a shared lag/fuzz shaper.

	Port of firmwares/mod1-switchblade/mod1-switchblade.ino, itself a port of Sean
	Luke's GRAINS `switchblade` (github.com/eclab/grains).

	F1 arrives through the LEVEL attenuator, F2 joins it at unity, the sum is
	de-glitched by a 3-tap median, then either lag-smoothed or roughened with
	noise depending on which side of centre SHAPE sits, then scaled and
	optionally flipped by the attenuverter. Lag and fuzz cannot both be on: they
	share one dial, which is the original's central compromise and is kept.

	Panel layout (mirrors the MOD1 hardware: 3 pots, 1 button, 1 LED, 4 jacks):
		POT1 LEVEL   — attenuates F1, or is a manual CV level when MAN is latched
		POT2 ±       — attenuvert: CCW fully inverted, centre silent, CW normal
		POT3 SHAPE   — CCW half lag-smooths, CW half adds noise
		BUTTON       — latch: MAN, i.e. LEVEL is a level of its own and F1 is ignored
		F1 in        — CV A, 0..10 V, through LEVEL
		F2 in        — CV B, 0..10 V, summed at unity
		F3 out       — processed CV (0..10 V, resting at 5 V)
		F4 out       — the same CV mirrored about 5 V
		LED          — brightness tracks the F3 output

	Everything between the jacks is sc::SwitchbladeVoice, shared verbatim with the
	firmware; this file owns only Rack I/O.

	Divergences from the firmware, all of them consequences of the medium:

	  - The button LATCHES rather than toggling a remembered flag. A mouse cannot
	    read an LED flash, and Rack persists parameters with the patch, so the
	    mode is simply visible in the switch position — which is closer to the
	    hardware slide switch on GRAINS that this button stands in for anyway.

	  - Voltages, not PWM. The firmware writes 10-bit Timer1 PWM to F3 and 8-bit
	    Timer2 to F4; here both are plain voltages, so the port has none of the
	    resolution ceiling either the original's 488 Mozzi steps or the MOD1
	    rewrite's 1024 imposes. The engine's own 256 Hz control rate is the only
	    quantisation left, and that one is deliberate — see SwitchbladeCore.h.

	  - Unpatched inputs read 0 V, matching a MOD1 jack with nothing in it, so an
	    empty module sits at 5 V until you turn LEVEL up in MAN mode.

	License:
	Apache License 2.0, Copyright 2023 Sean Luke (sean@cs.gmu.edu) — the engine
	here and in SwitchbladeCore.h derives from the GRAINS `switchblade` firmware,
	and Apache 2.0 permits modification and redistribution (including
	commercially) only so long as the license notice ships with every copy and
	modified files carry prominent notice of change. The notice is kept at
	firmwares/mod1-switchblade/LICENSE.md and the firmware sketch's "Deliberate
	changes from the original" list is the notice of changes; keep both and ship
	them. Unlike the CC0 HAGIWO modules in this plugin, this one carries
	conditions.
	The Rack-side work in this file is MIT License, Copyright (c) 2026 Madelyn
	Yeary, see rack-plugins/LICENSE.md.
*/

struct Switchblade : Module {
	enum ParamId {
		LEVEL_PARAM,
		ATTENUVERT_PARAM,
		SHAPE_PARAM,
		MANUAL_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		F1_INPUT,   // CV A, through LEVEL
		F2_INPUT,   // CV B, summed at unity
		INPUTS_LEN
	};
	enum OutputId {
		F3_OUTPUT,  // processed CV (0..10V)
		F4_OUTPUT,  // mirrored about 5V
		OUTPUTS_LEN
	};
	enum LightId {
		OUT_LIGHT,
		LIGHTS_LEN
	};

	sc::SwitchbladeVoice voice;

	Switchblade() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

		configParam(LEVEL_PARAM, 0.f, 1.f, 1.f, "Level (F1 attenuation)", "%", 0.f, 100.f);
		// Centre is silence and the ends are unity with a sign, so the display
		// reads as the gain the knob actually applies.
		configParam(ATTENUVERT_PARAM, 0.f, 1.f, 1.f, "Attenuvert", "%", 0.f, 200.f, -100.f);
		configParam(SHAPE_PARAM, 0.f, 1.f, 0.5f, "Shape (CCW lag / CW fuzz)");
		configSwitch(MANUAL_PARAM, 0.f, 1.f, 0.f, "Level source", {"F1 input", "Manual"});

		configInput(F1_INPUT, "F1 CV A (through Level)");
		configInput(F2_INPUT, "F2 CV B (unity)");
		configOutput(F3_OUTPUT, "F3 processed CV");
		configOutput(F4_OUTPUT, "F4 inverted CV");
		configLight(OUT_LIGHT, "Output level");
	}

	void onReset() override {
		voice.reset();
	}

	void process(const ProcessArgs& args) override {
		// Latch stands in for the firmware's button-toggled MAN mode.
		voice.manualMode = params[MANUAL_PARAM].getValue() > 0.5f;

		const sc::SwitchbladeParams p = sc::switchbladeMapParams(
			params[LEVEL_PARAM].getValue(),
			params[ATTENUVERT_PARAM].getValue(),
			params[SHAPE_PARAM].getValue());

		// The engine works in the 0..1 unipolar CV the MOD1 ADC hands it, so
		// 0..10 V maps straight onto that range. An unpatched jack reads 0 V,
		// which is what an empty MOD1 jack reads too.
		const float inA = clamp(inputs[F1_INPUT].getVoltage() / 10.f, 0.f, 1.f);
		const float inB = clamp(inputs[F2_INPUT].getVoltage() / 10.f, 0.f, 1.f);

		// The engine ticks itself at its own 256 Hz control rate and holds
		// between ticks, so it is safe (and correct) to call it every sample.
		voice.process(args.sampleTime, inA, inB, p);

		outputs[F3_OUTPUT].setVoltage(voice.out() * 10.f);
		outputs[F4_OUTPUT].setVoltage(voice.inverted() * 10.f);

		lights[OUT_LIGHT].setBrightness(voice.out());
	}
};

struct SwitchbladeWidget : ModuleWidget {
	SwitchbladeWidget(Switchblade* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/mod1-switchblade.svg")));

		// 4 HP Mod1 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		// Three pots. Reversed<> because the MOD1 panel pots are wired backwards.
		addParam(createParamCentered<Reversed<RoundBlackKnob>>(mm2px(Vec(10.03f, 21.7f)), module, Switchblade::LEVEL_PARAM));
		addParam(createParamCentered<Reversed<RoundBlackKnob>>(mm2px(Vec(10.04f, 40.06f)), module, Switchblade::ATTENUVERT_PARAM));
		addParam(createParamCentered<Reversed<RoundBlackKnob>>(mm2px(Vec(10.04f, 58.42f)), module, Switchblade::SHAPE_PARAM));

		// Latching MAN button + output LED.
		addParam(createParamCentered<VCVLatch>(mm2px(Vec(5.19f, 78.57f)), module, Switchblade::MANUAL_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, Switchblade::OUT_LIGHT));

		// Jacks: F1/F2 in on the top row, F3/F4 out on the bottom.
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, Switchblade::F1_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.3f)), module, Switchblade::F2_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, Switchblade::F3_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, Switchblade::F4_OUTPUT));
	}
};

Model* modelSwitchblade = createModel<Switchblade, SwitchbladeWidget>("mod1-switchblade");
