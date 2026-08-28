#include "plugin.hpp"
#include <BookerCore.h>  // Shared Booker voice (also used by the mod2-booker firmware)

/*
	Booker — Hammond tonewheel organ with a Leslie, as nine additive drawbars.

	Port of firmwares/mod2-booker/mod2-booker.ino (HAGIWO Mod2, RP2350), itself a
	port of Sean Luke's GRAINS `booker` firmware (github.com/eclab/grains), used
	under the Apache License 2.0, Copyright 2023 Sean Luke. The upstream notice
	ships beside the firmware as firmwares/mod2-booker/LICENSE.md and Apache 2.0
	requires it to travel with every copy and modified files to carry notice of
	their changes — unlike the CC0 HAGIWO modules in this plugin, which carry no
	such condition.

	Mirrors the Mod2 hardware: 3 pots, 1 push button, 1 LED, and the Mod2 jack
	set (IN1, IN2, CV, OUT). For the Booker firmware:
		POT1   -> Registration: which of the 16 drawbar settings is pulled out
		POT2   -> Volume
		POT3   -> Tune (0 .. +4.94 octaves above upstream's C0, 32.7 Hz)
		BUTTON -> Leslie: cycles OFF / chorale (0.66 Hz) / tremolo (5.66 Hz)
		LED    -> Note level, dipping once per rotor turn
		IN1    -> Key gate
		IN2    -> Leslie speed footswitch: high forces tremolo
		CV     -> 1V/Oct pitch
		OUT    -> audio output

	Everything musical — the nine tonewheel ratios, the 16 registrations, the
	drawbar taper, the level relationship that makes Full Organ overdrive, and
	the Leslie — is sc::BookerVoice, shared verbatim with the firmware. This file
	owns only Rack I/O.

	Three divergences from the firmware, all of them because Rack knows things
	the panel cannot:

	  - IN1 is normalled high. GRAINS drones and has no gate at all, so the
	    firmware needs a long-press mode to choose between droning and gating —
	    an unpatched jack on the hardware reads low and would leave the module
	    silently dead. Here an unpatched IN1 simply holds the key down, so the
	    module drones like the original until you patch a gate into it, and the
	    mode has nothing left to decide.
	  - Registration is a 16-position switch, not a continuous pot, so the
	    tooltip can name the setting you are on. The firmware quantises the same
	    pot to the same 16 values; this only puts detents on it.
	  - 1V/Oct is standard non-inverted. The firmware reads a negative-slope,
	    calibration-trimmed A2 because of the hardware's input divider (and
	    GRAINS before it tracked 1.3 V/octave); this tracks a sequencer directly.
	    The TUNE knob covers the same ~4.94 octaves the panel pot does.

	License:
	Apache License 2.0 for the engine: the original Booker firmware is Copyright
	2023 Sean Luke (sean@cs.gmu.edu), from the GRAINS project, Apache 2.0. Its
	notice lives at firmwares/mod2-booker/LICENSE.md and must ship with any copy
	of the engine; the firmware header's "Deliberate changes from the original"
	list plus the divergences above are the required notice of changes.

	The Rack-side work in this file is MIT License, Copyright (c) 2026 Madelyn
	Yeary, see rack-plugins/LICENSE.md.
*/

struct Booker : Mod2Module {
	enum ParamId {
		REG_PARAM,
		VOL_PARAM,
		TUNE_PARAM,
		LESLIE_PARAM,  // momentary button
		PARAMS_LEN
	};
	enum InputId {
		GATE_INPUT,    // IN1 — key gate, normalled high
		LESLIE_INPUT,  // IN2 — speed footswitch
		CV_INPUT,      // 1V/Oct pitch
		INPUTS_LEN
	};
	enum OutputId {
		AUDIO_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		LEVEL_LIGHT,
		LIGHTS_LEN
	};

	// Pot span, matching the firmware's A2 calibration (8.3 * 33/55 * 0.992).
	static constexpr float TUNE_SPAN_OCT = 4.94f;

	// Voice state lives in the shared core (same synthesis as the firmware).
	sc::BookerVoice core;

	dsp::BooleanTrigger leslieButton;
	dsp::SchmittTrigger gateTrigger;   // IN1 level, with hysteresis
	dsp::SchmittTrigger speedTrigger;  // IN2 level, with hysteresis
	int leslieSetting = sc::BOOKER_LESLIE_FAST;  // upstream ships the Leslie on

	Booker() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		// Upstream's 16 shipped registrations, in its order. The digits are the
		// drawbar stops; the names are the players and songs they belong to.
		configSwitch(REG_PARAM, 0.f, 15.f, 0.f, "Registration", {
			"888888888 Full Organ",
			"885324588 Blues",
			"888800000 Booker T. Jones 1",
			"888630000 Booker T. Jones 2",
			"878000456 Bright Comping",
			"843000000 Dark Comping",
			"808808008 Gospel 1",
			"888000008 Gospel 2",
			"868666568 Greg Allman 1",
			"888600000 Greg Allman 2",
			"886800300 Paul Shaffer 1",
			"888768888 Paul Shaffer 2",
			"888878678 Paul Shaffer 3",
			"808000008 Reggae",
			"080000000 Sine",
			"876543211 Strings",
		});
		// Full Organ past about 43 % runs into the soft-clip, which is upstream's
		// own balance rather than a limit — that overdrive is the sound.
		configParam(VOL_PARAM, 0.f, 1.f, 0.4f, "Volume", "%", 0.f, 100.f);
		configParam(TUNE_PARAM, 0.f, TUNE_SPAN_OCT, 2.f, "Tune", " oct");
		configButton(LESLIE_PARAM, "Leslie (off / chorale / tremolo)");

		configInput(GATE_INPUT, "Key gate (unpatched = drone)");
		configInput(LESLIE_INPUT, "Leslie fast (gate)");
		configInput(CV_INPUT, "1V/Oct pitch");
		configOutput(AUDIO_OUTPUT, "Audio");
	}

	void onReset() override {
		core.reset();
		leslieSetting = sc::BOOKER_LESLIE_FAST;
	}

	// Mod2Module's panelStyle has to be merged in by hand once a module keeps
	// JSON of its own.
	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "leslieSetting", json_integer(leslieSetting));
		mod2WritePanelStyle(root, panelStyle);
		return root;
	}
	void dataFromJson(json_t* root) override {
		if (json_t* j = json_object_get(root, "leslieSetting"))
			leslieSetting = clamp((int) json_integer_value(j),
			                      (int) sc::BOOKER_LESLIE_OFF, (int) sc::BOOKER_LESLIE_FAST);
		mod2ReadPanelStyle(root, panelStyle);
	}

	void process(const ProcessArgs& args) override {
		// Leslie button: OFF -> chorale -> tremolo (matches the firmware's cycle).
		if (leslieButton.process(params[LESLIE_PARAM].getValue() > 0.5f))
			leslieSetting = (leslieSetting + 1) % (sc::BOOKER_LESLIE_FAST + 1);

		// IN2 is the cabinet's speed pedal: it can hurry a running rotor but not
		// start a stopped one, exactly as on the firmware.
		// Both jacks are levels, not edges, so the triggers are only here for
		// their hysteresis — isHigh() is the state, process() just updates it.
		speedTrigger.process(inputs[LESLIE_INPUT].getVoltage(), 0.1f, 1.f);
		const bool fastPedal = speedTrigger.isHigh();
		core.leslieMode = (leslieSetting == sc::BOOKER_LESLIE_OFF) ? sc::BOOKER_LESLIE_OFF
		                  : fastPedal                              ? sc::BOOKER_LESLIE_FAST
		                                                           : leslieSetting;

		// Key gate. An unpatched IN1 normals high, so the module drones like the
		// GRAINS original until a gate is actually patched in.
		gateTrigger.process(inputs[GATE_INPUT].getNormalVoltage(10.f), 0.1f, 1.f);
		core.gate = gateTrigger.isHigh();

		core.setRegistration((int) std::round(params[REG_PARAM].getValue()));
		core.volume = params[VOL_PARAM].getValue();

		// Pitch: TUNE knob in octaves above upstream's 0 V note (C0, 32.7 Hz at
		// the 16' drawbar), plus a standard 1V/Oct CV.
		const float volts = params[TUNE_PARAM].getValue() + inputs[CV_INPUT].getVoltage();
		core.freq = clamp(sc::bookerFreqFromVolts(volts), 1.f, 8000.f);

		// One sample from the shared core: audio in -1..+1 -> ±5 V (Vpp 10 V).
		const sc::BookerFrame f = core.process(args.sampleTime);
		outputs[AUDIO_OUTPUT].setVoltage(f.audio * 5.f);
		lights[LEVEL_LIGHT].setBrightness(f.env);
	}
};

struct BookerWidget : ModuleWidget {
	BookerWidget(Booker* module) {
		setModule(module);
		setMod2Panel(this, module, "res/mod2-booker.svg");
		// 4 HP Mod1/Mod2 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<Reversed<RoundBlackKnob>>(mm2px(Vec(10.03f, 21.7f)), module, Booker::REG_PARAM));
		addParam(createParamCentered<Reversed<RoundBlackKnob>>(mm2px(Vec(10.04f, 40.06f)), module, Booker::VOL_PARAM));
		addParam(createParamCentered<Reversed<RoundBlackKnob>>(mm2px(Vec(10.04f, 58.42f)), module, Booker::TUNE_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, Booker::LESLIE_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, Booker::LEVEL_LIGHT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, Booker::GATE_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.3f)), module, Booker::LESLIE_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, Booker::AUDIO_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, Booker::CV_INPUT));
	}

	void appendContextMenu(Menu* menu) override {
		appendMod2PanelMenu(menu, module);
	}
};

Model* modelBooker = createModel<Booker, BookerWidget>("mod2-booker");
