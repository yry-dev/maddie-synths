#include "plugin.hpp"
#include <MemoirCore.h>  // Shared Memoir recorder (also used by the firmware)

/*
	Memoir — a 4 to 32 second CV and gate recorder.

	Port of firmwares/mod1-memoir/mod1-memoir.ino, itself a port of Sean Luke's
	`memoir` from the GRAINS collection. A take is 512 frames of 9-bit CV plus a
	one-bit gate track; the LENGTH knob is the frame rate, so the same 512 frames
	stretch over 4 seconds or 32. Because LENGTH is read live rather than latched
	with the take, recording short and replaying long (or the reverse) is the
	instrument, not a bug.

	The recorder itself — buffer, packing, transport, the tail-padding that holds
	a short take's last value — is sc::MemoirEngine, shared verbatim with the
	firmware. This file owns only Rack I/O and the patch-JSON persistence that
	stands in for the module's EEPROM.

		POT1 / CV   -> CV in offset, sums with the F1 jack
		POT2 / GATE -> gate in offset, sums with the F2 jack
		POT3 / LEN  -> take length: 4, 8, 12 … 32 seconds
		LED         -> blinks while recording, follows the CV on playback
		F1 in       -> CV (0..5 V, summed with POT1 and clamped)
		F2 in       -> gate (hysteretic at the firmware's 1.95 V / 2.93 V)
		F3 out      -> CV (0..5 V, held between frames)
		F4 out      -> gate

	Divergences from the firmware, both of them about hands:

	  - The hardware has one button and distinguishes a tap (play) from a 600 ms
	    hold (erase and record). A mouse is bad at held gestures and there is no
	    tactile feedback to time them against, so the two gestures are split. The
	    panel has one button hole, and it goes to the tap gesture — play — while
	    RECORD moves to the right-click menu. That keeps the two apart in the way
	    the firmware intended: the gesture that erases the take is the deliberate
	    one, and here it is the one you cannot reach by accident.
	  - Committing a take on the hardware means writing 1024 EEPROM bytes, which
	    stalls the sketch for a second or two; here the engine's store request is
	    consumed immediately and the buffer rides along in the patch, so a take
	    survives a save/load the way it survives a power cycle on the module.

	The CV path is unipolar 0..5 V in both directions, matching the module's ADC
	and filtered PWM, so recording and playback are unity.

	License:
	Apache License 2.0, Copyright 2023 Sean Luke (sean@cs.gmu.edu) — this module
	and sc::MemoirEngine are derived from `memoir` in the GRAINS project
	(github.com/eclab/grains). Apache requires the license notice to travel with
	the code and modified files to carry prominent notice of changes; the notice
	is kept at firmwares/mod1-memoir/LICENSE.md and the firmware header carries
	the list of changes. Keep them there and ship them — the CC0 HAGIWO modules
	elsewhere in this plugin carry no such condition.
	The Rack-side work in this file is MIT License, Copyright (c) 2026 Madelyn
	Yeary, see rack-plugins/LICENSE.md.
*/

struct Memoir : Module {
	enum ParamId {
		CV_PARAM,
		GATE_PARAM,
		LENGTH_PARAM,
		REC_PARAM,
		PLAY_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		F1_INPUT,   // CV
		F2_INPUT,   // gate
		INPUTS_LEN
	};
	enum OutputId {
		F3_OUTPUT,  // CV
		F4_OUTPUT,  // gate
		OUTPUTS_LEN
	};
	enum LightId {
		LED_LIGHT,
		LIGHTS_LEN
	};

	sc::MemoirEngine engine;
	dsp::BooleanTrigger recTrigger;
	dsp::BooleanTrigger playTrigger;

	// The gate track is read as a level with hysteresis rather than as an edge,
	// so it holds between frames exactly as the firmware's ADC comparison does.
	bool gateIn = false;

	Memoir() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(CV_PARAM, 0.f, 1.f, 0.f, "CV in offset", " V", 0.f, 5.f);
		configParam(GATE_PARAM, 0.f, 1.f, 0.f, "Gate in offset", " V", 0.f, 5.f);
		// Detented to eight positions, which is all the hardware pot resolves:
		// the firmware divides its ADC into the same eight zones.
		configSwitch(LENGTH_PARAM, 0.f, 7.f, 0.f, "Length",
		             {"4 s", "8 s", "12 s", "16 s", "20 s", "24 s", "28 s", "32 s"});
		// REC has no panel hole, so it is pressed from the context menu; PLAY is
		// the button the hardware has.
		configButton(REC_PARAM, "Record (erases the current take)");
		configButton(PLAY_PARAM, "Play / restart");

		configInput(F1_INPUT, "F1 CV");
		configInput(F2_INPUT, "F2 gate");
		configOutput(F3_OUTPUT, "F3 CV");
		configOutput(F4_OUTPUT, "F4 gate");

		engine.erase();
		engine.reset();
	}

	// Initialize is the factory reset the hardware does not have: it wipes the
	// take as well as the transport.
	void onReset() override {
		engine.reset();
		engine.erase();
		gateIn = false;
	}

	void process(const ProcessArgs& args) override {
		// Nothing releases REC — it is a menu item, not a panel button — so the
		// press is dropped back here once it has been taken.
		if (recTrigger.process(params[REC_PARAM].getValue() > 0.5f)) {
			engine.triggerRecord();
			params[REC_PARAM].setValue(0.f);
		}
		if (playTrigger.process(params[PLAY_PARAM].getValue() > 0.5f))
			engine.triggerPlay();

		// Pot + jack, clamped — the firmware's addClamp1023(pot, cv) on a 0..5 V
		// scale.
		const float cvIn = clamp(params[CV_PARAM].getValue()
		                       + inputs[F1_INPUT].getVoltage() / 5.f, 0.f, 1.f);
		const float gateLevel = clamp(params[GATE_PARAM].getValue()
		                            + inputs[F2_INPUT].getVoltage() / 5.f, 0.f, 1.f);
		// 600 and 400 of a 10-bit ADC, the firmware's thresholds.
		if (gateLevel > 600.f / 1023.f)
			gateIn = true;
		else if (gateLevel < 400.f / 1023.f)
			gateIn = false;

		engine.setRate((uint8_t) params[LENGTH_PARAM].getValue() + 1);

		const sc::MemoirFrame f = engine.process(args.sampleTime, cvIn, gateIn);

		// Nothing to flush — the buffer is serialised with the patch — but the
		// request still has to be taken or it stays raised.
		engine.takeStoreRequest();

		outputs[F3_OUTPUT].setVoltage(f.cv * 5.f);
		outputs[F4_OUTPUT].setVoltage(f.gate ? 10.f : 0.f);
		lights[LED_LIGHT].setBrightnessSmooth(f.led, args.sampleTime);
	}

	// The take is the module's EEPROM: it belongs in the patch, not in RAM only.
	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_t* takeJ = json_array();
		const uint8_t* bytes = engine.bytes();
		for (uint16_t i = 0; i < engine.byteCount(); i++)
			json_array_append_new(takeJ, json_integer(bytes[i]));
		json_object_set_new(rootJ, "take", takeJ);
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* takeJ = json_object_get(rootJ, "take");
		if (!takeJ || !json_is_array(takeJ))
			return;
		uint8_t* bytes = engine.bytes();
		size_t n = json_array_size(takeJ);
		if (n > (size_t) engine.byteCount())
			n = (size_t) engine.byteCount();
		for (size_t i = 0; i < n; i++)
			bytes[i] = (uint8_t) json_integer_value(json_array_get(takeJ, i));
	}
};

struct MemoirWidget : ModuleWidget {
	MemoirWidget(Memoir* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/mod1-memoir.svg")));

		// 4 HP Mod1 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		// Reversed<> because the Mod1 pots are wired backwards on the panel.
		addParam(createParamCentered<Reversed<RoundBlackKnob>>(mm2px(Vec(10.03f, 21.7f)), module, Memoir::CV_PARAM));
		addParam(createParamCentered<Reversed<RoundBlackKnob>>(mm2px(Vec(10.04f, 40.06f)), module, Memoir::GATE_PARAM));
		addParam(createParamCentered<Reversed<RoundBlackKnob>>(mm2px(Vec(10.04f, 58.42f)), module, Memoir::LENGTH_PARAM));

		// The panel's one button hole takes the hardware's tap gesture; REC is in
		// the context menu.
		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, Memoir::PLAY_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, Memoir::LED_LIGHT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, Memoir::F1_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.3f)), module, Memoir::F2_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, Memoir::F3_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, Memoir::F4_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Memoir* m = dynamic_cast<Memoir*>(module);
		assert(m);
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuItem("Record (erases the current take)", "",
			[=]() { m->params[Memoir::REC_PARAM].setValue(1.f); }));
	}
};

Model* modelMemoir = createModel<Memoir, MemoirWidget>("mod1-memoir");
