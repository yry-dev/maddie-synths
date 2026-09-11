#include "plugin.hpp"
#include <ArpCore.h>  // Shared arpeggiator engine (also used by the firmware)

/*
	Arp — clocked arpeggiator with 1 V/oct pitch out.

	Port of firmwares/mod1-arp/mod1-arp.ino, itself a port of Sean Luke's GRAINS
	`arp`. The chord tables, the twelve traversal styles and the inversion rule
	live in sc::ArpEngine; this file owns only the Rack I/O.

	Mirrors the Mod1 hardware: 3 pots, 1 push button, 1 PWM LED and 4 flexible
	jacks (F1..F4). For the Arp firmware:
		POT1  -> Pitch (root of the arpeggio, 0-35 semitones)
		POT2  -> Chord (16 chords)
		POT3  -> Style (12 patterns)
		BUTTON-> taps through inversion 0-3 (persisted to EEPROM)
		LED   -> follows the pitch CV level
		F1    -> clock in; the arpeggio advances on the rising edge
		F2    -> pitch CV in (sums with POT1)
		F3    -> pitch CV out, 1 V/oct
		F4    -> gate out, the clock delayed by ~2 ms

	Divergences from the firmware:

	- The button becomes a 4-position INVERSION selector. On the hardware a tap
	  cycles 0-3 and a hold restarts the arpeggio; a Rack control cannot express
	  a hold, so it exposes the tap function directly (the same choice
	  rabid-audio-clk made for its held buttons). The restart gesture has no
	  Rack equivalent, and needs none: the engine already restarts whenever the
	  chord, style or inversion changes — which is upstream's behaviour, not a
	  new one — and "Initialize" restarts it outright.

	- The pitch INPUT is deliberately not 1 V/oct. 0-5 V spans the whole 3-octave
	  root range, about 7.2 semitones per volt, because that is what summing a CV
	  onto a pot through a 10-bit ADC does on the hardware. Upstream was blunter
	  about it ("Grains is odd in that the input to IN1 isn't necessarily
	  1V/oct. You'll have to tune it") and gave you POT1 as the trim. The pitch
	  OUTPUT is true 1 V/oct on both platforms.

	- Pitch leaves at 0-5 V rather than the 0-10 V the other Mod1 ports use for
	  unipolar CV. 1 V/oct is an absolute scale: the engine's five-octave span
	  has to be five volts or nothing tracks.

	License:
	Apache License 2.0 for the engine. The original Arp firmware is Copyright
	2025 Sean Luke (sean@cs.gmu.edu), from the GRAINS project
	(github.com/eclab/grains), Apache 2.0. Apache requires the license notice to
	travel with the code and modified files to carry prominent notice of
	changes: the notice lives at firmwares/mod1-arp/LICENSE.md and the divergence
	lists here and in the sketch header are the notice of changes. Keep both.

	The Rack-side work in this file is MIT License, Copyright (c) 2026 Madelyn
	Yeary, see rack-plugins/LICENSE.md.
*/

struct Arp : Module {
	enum ParamId {
		PITCH_PARAM,
		CHORD_PARAM,
		STYLE_PARAM,
		INVERSION_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		F1_CLOCK_INPUT,
		F2_PITCH_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		F3_PITCH_OUTPUT,
		F4_GATE_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		LED_LIGHT,
		LIGHTS_LEN
	};

	sc::ArpEngine arp;
	dsp::SchmittTrigger clockTrigger;

	// The gate trails the clock's rising edge so an envelope reads the new note
	// rather than the one it replaced; it drops as soon as the clock does.
	float gateDelayRemaining = 0.f;
	bool gateArmed = false;
	bool gateHigh = false;

	Arp() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(PITCH_PARAM, 0.f, 1.f, 0.f, "Pitch (root)");
		configParam(CHORD_PARAM, 0.f, 1.f, 0.f, "Chord");
		configParam(STYLE_PARAM, 0.f, 1.f, 0.f, "Style");
		configSwitch(INVERSION_PARAM, 0.f, 3.f, 0.f, "Inversion",
		             {"Root position", "1st", "2nd", "3rd"});

		configInput(F1_CLOCK_INPUT, "F1 clock");
		configInput(F2_PITCH_INPUT, "F2 pitch CV");
		configOutput(F3_PITCH_OUTPUT, "F3 pitch (1V/oct)");
		configOutput(F4_GATE_OUTPUT, "F4 gate");
	}

	void onReset() override {
		arp.reset();
		gateArmed = false;
		gateHigh = false;
		gateDelayRemaining = 0.f;
	}

	void process(const ProcessArgs& args) override {
		// Sum pot (0..1) + CV (0..5 V → 0..1), clamped — the firmware's
		// addClamp1023(pot_adc, cv_adc) / 1023.
		const float pitch01 = clamp(params[PITCH_PARAM].getValue()
		                          + inputs[F2_PITCH_INPUT].getVoltage() / 5.f, 0.f, 1.f);
		const uint8_t root = sc::arpRootSemitone(pitch01);

		if (clockTrigger.process(inputs[F1_CLOCK_INPUT].getVoltage(), 0.1f, 1.f)) {
			arp.step(sc::arpSelectChord(params[CHORD_PARAM].getValue()),
			         sc::arpSelectStyle(params[STYLE_PARAM].getValue()),
			         (uint8_t)params[INVERSION_PARAM].getValue());
			gateDelayRemaining = sc::kArpGateDelaySec;
			gateArmed = true;
			gateHigh = false;
		}

		if (!clockTrigger.isHigh()) {
			gateArmed = false;
			gateHigh = false;
		}
		else if (gateArmed) {
			gateDelayRemaining -= args.sampleTime;
			if (gateDelayRemaining <= 0.f) {
				gateArmed = false;
				gateHigh = true;
			}
		}

		// Emitted every sample, not only on the clock, so moving the pitch CV
		// transposes the note already sounding. The firmware does the same at
		// its control rate; upstream got it free by rebuilding at audio rate.
		outputs[F3_PITCH_OUTPUT].setVoltage((float)arp.pitchSemitone(root) / 12.f);
		outputs[F4_GATE_OUTPUT].setVoltage(gateHigh ? 10.f : 0.f);

		lights[LED_LIGHT].setBrightness(arp.pitchCv(root));
	}
};

struct ArpWidget : ModuleWidget {
	ArpWidget(Arp* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/mod1-arp.svg")));
		// 4 HP Mod1/Mod2 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<Reversed<RoundBlackKnob>>(mm2px(Vec(10.03f, 21.7f)), module, Arp::PITCH_PARAM));
		addParam(createParamCentered<Reversed<RoundBlackKnob>>(mm2px(Vec(10.04f, 40.06f)), module, Arp::CHORD_PARAM));
		addParam(createParamCentered<Reversed<RoundBlackKnob>>(mm2px(Vec(10.04f, 58.42f)), module, Arp::STYLE_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(5.19f, 78.57f)), module, Arp::INVERSION_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, Arp::LED_LIGHT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, Arp::F1_CLOCK_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.3f)), module, Arp::F2_PITCH_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, Arp::F3_PITCH_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, Arp::F4_GATE_OUTPUT));
	}
};

Model* modelArp = createModel<Arp, ArpWidget>("mod1-arp");
