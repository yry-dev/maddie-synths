#include "plugin.hpp"
#include <DropletsCore.h>  // Shared Droplets voice (also used by the firmware)

/*
	Droplets — a wind chime: random sine notes drawn from a chord, left to ring.

	Port of firmwares/mod2-droplets/mod2-droplets.ino, itself a port of Sean
	Luke's GRAINS `droplets`. The chord tables, the draw, the fade-in and the
	linear ring-down are all sc::DropletsVoice, shared verbatim with the
	firmware; this file owns only the Rack I/O.

	Mirrors the Mod2 panel (3 pots, 1 button, 1 LED, 4 jacks):
		POT1 CHORD -> which of twelve chords the droplets are drawn from
		POT2 RANGE -> voicing width AND release, in ten steps (see below)
		POT3 ROOT  -> root pitch, 0..5 octaves above C0 (32.7 Hz)
		BUTTON     -> short click transposes 0/+1/+2/+3; hold drops one droplet
		LED        -> droplet envelope, over a resting glow showing the transpose
		IN1        -> trigger: one droplet per rising edge
		IN2        -> accent gate: droplets struck while high are half volume
		CV         -> 1 V/oct root pitch, summed with the ROOT knob
		OUT        -> audio

	RANGE is the knob worth understanding, and it is upstream's design rather
	than ours: its lower half selects the wide voicings that spread droplets over
	three or four octaves, its upper half the same chords folded into about half
	that span, and within each half the five positions run from a ~0.12 s tick to
	a ~2 s chime. So sweeping it crosses from short and wide to long and narrow
	twice, and both halves are musically useful.

	Four droplets can ring at once and a fifth steals the oldest, which is what
	keeps a fast trigger from turning into a wall — it thins itself out.

	Divergences from the firmware:
	  - ROOT is a knob and the CV jack is separate. On the Mod2 those are one ADC
	    pin and cannot be told apart; Rack has a real jack, so ROOT is a 0..5
	    octave offset and CV is plain 1 V/oct summed on top. 0 V with ROOT at
	    zero is C0, upstream's documented tuning, so a sequencer patched in
	    tracks exactly as GRAINS did.
	  - The button's long press is a mouse-down-and-hold here, which a mouse can
	    express; it fires the droplet the moment the hold threshold passes, and
	    the release then does not transpose.
	  - Droplets are capped at Rack's Nyquist rather than the firmware's, so the
	    frequency ceiling follows the host sample rate.

	The firmware header lists the divergences from GRAINS itself (chord select
	moved onto the freed pitch-trim pot, 1 V/oct CV, transpose on the button,
	accent on IN2); the musical ones live in the shared core, so this port
	inherits them.

	License:
	Apache License 2.0, Copyright 2023 Sean Luke (sean@cs.gmu.edu) — this module
	and its sc::DropletsVoice are derived from the GRAINS `droplets` firmware
	(github.com/eclab/grains), and Apache 2.0 permits modification and
	redistribution only so long as the license notice ships with every copy or
	substantial portion of the work, and modified files carry prominent notice of
	change. That notice is kept at firmwares/mod2-droplets/LICENSE.md; keep it
	there and ship it. Unlike the CC0 HAGIWO modules in this plugin, this one
	carries conditions.
	The Rack-side work in this file is MIT License, Copyright (c) 2026 Madelyn
	Yeary, see rack-plugins/LICENSE.md.
*/

// Read the chord name out of the shared core so the tooltip and the table can
// never disagree.
struct DropletsChordQuantity : ParamQuantity {
	std::string getDisplayValueString() override {
		return sc::dropletsChordName(sc::dropletsChordSelect(getValue()));
	}
};

// RANGE carries two things, so spell both out rather than showing a bare 0..1.
struct DropletsRangeQuantity : ParamQuantity {
	std::string getDisplayValueString() override {
		int sel = sc::dropletsRangeSelect(getValue());
		const char* width = (sel > 4) ? "narrow" : "wide";
		if (sel > 4)
			sel -= 5;
		return string::f("%s, %.2f s", width, sc::kDropletReleaseSec[sel]);
	}
};

struct Droplets : Mod2Module {
	enum ParamId {
		CHORD_PARAM,
		RANGE_PARAM,
		ROOT_PARAM,
		OCT_PARAM,  // button: click transposes, hold drops a droplet
		PARAMS_LEN
	};
	enum InputId {
		TRIG_INPUT,    // IN1
		ACCENT_INPUT,  // IN2
		CV_INPUT,      // 1 V/oct root pitch
		INPUTS_LEN
	};
	enum OutputId { AUDIO_OUTPUT, OUTPUTS_LEN };
	enum LightId { ENV_LIGHT, LIGHTS_LEN };

	// The whole model lives in the shared core (same code as the firmware).
	sc::DropletsVoice voice;

	dsp::SchmittTrigger gateTrigger;

	int octShift = 0;      // 0..3, the firmware's flash-saved transpose
	float btnHeld = 0.f;   // seconds the button has been down
	bool btnFired = false; // this press already dropped a droplet

	// How far ROOT reaches on its own, matching the firmware's pot span.
	static constexpr float ROOT_POT_OCTAVES = 5.f;
	// Accent attenuation, matching the firmware and the other Mod2 voices.
	static constexpr float ACCENT_LEVEL = 0.5f;
	// Resting LED brightness per octave of transpose (the firmware's OCT_GLOW).
	static constexpr float OCT_GLOW = 0.1f;

	Droplets() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam<DropletsChordQuantity>(CHORD_PARAM, 0.f, 1.f, 0.f, "Chord");
		configParam<DropletsRangeQuantity>(RANGE_PARAM, 0.f, 1.f, 0.f, "Range & release");
		// Default 0 so a sequencer patched into CV plays upstream's tuning
		// straight away: 0 V is C0.
		configParam(ROOT_PARAM, 0.f, 1.f, 0.f, "Root pitch", " oct above C0", 0.f,
		            ROOT_POT_OCTAVES);
		configButton(OCT_PARAM, "Octave 0/+1/+2/+3 (hold: drop one droplet)");

		configInput(TRIG_INPUT, "IN1 trigger (one droplet per rising edge)");
		configInput(ACCENT_INPUT, "IN2 accent gate (half-volume droplets)");
		configInput(CV_INPUT, "1V/Oct root pitch");
		configOutput(AUDIO_OUTPUT, "Audio");
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "octShift", json_integer(octShift));
		mod2WritePanelStyle(root, panelStyle);
		return root;
	}
	void dataFromJson(json_t* root) override {
		if (json_t* o = json_object_get(root, "octShift"))
			octShift = clamp((int)json_integer_value(o), 0, 3);
		mod2ReadPanelStyle(root, panelStyle);
	}

	void onReset() override {
		voice.reset();
		octShift = 0;
		btnHeld = 0.f;
		btnFired = false;
	}

	// Drop one droplet, latching the accent gate for its whole ring-down (the
	// firmware samples IN2 at the trigger edge for the same reason).
	void strike() {
		const float level = inputs[ACCENT_INPUT].getVoltage() > 1.f ? ACCENT_LEVEL : 1.f;
		voice.strike(level);
	}

	void process(const ProcessArgs& args) override {
		// Button: a click transposes on release, a hold drops a droplet as soon
		// as the threshold passes and then suppresses the transpose.
		if (params[OCT_PARAM].getValue() > 0.5f) {
			btnHeld += args.sampleTime;
			if (!btnFired && btnHeld >= sc::kDropletLongPressSec) {
				strike();
				btnFired = true;
			}
		}
		else {
			if (btnHeld > 0.f && !btnFired)
				octShift = (octShift + 1) & 3;
			btnHeld = 0.f;
			btnFired = false;
		}

		// Root pitch: knob offset + 1 V/oct + transpose, through the same mapping
		// (and the same 7.5-octave ceiling) the firmware uses.
		const float octaves = params[ROOT_PARAM].getValue() * ROOT_POT_OCTAVES +
		                      inputs[CV_INPUT].getVoltage() + (float) octShift;
		voice.maxHz = args.sampleRate * 0.5f;
		voice.setParams(params[CHORD_PARAM].getValue(), params[RANGE_PARAM].getValue(),
		                sc::dropletsRootFreq(octaves));

		if (gateTrigger.process(inputs[TRIG_INPUT].getVoltage(), 0.1f, 1.f))
			strike();

		// One sample from the shared core: audio in -1..+1 → ±5 V (Vpp 10 V).
		const sc::DropletsFrame f = voice.process(args.sampleTime);
		outputs[AUDIO_OUTPUT].setVoltage(f.audio * 5.f);

		// LED: droplet envelope over a resting glow that shows the transpose,
		// which is the only place that state is visible (as on the hardware).
		lights[ENV_LIGHT].setBrightnessSmooth(std::max(f.env, OCT_GLOW * octShift),
		                                      args.sampleTime);
	}
};

struct DropletsWidget : ModuleWidget {
	DropletsWidget(Droplets* module) {
		setModule(module);
		setMod2Panel(this, module, "res/mod2-droplets.svg");

		// 4 HP Mod1/Mod2 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<Reversed<RoundBlackKnob>>(mm2px(Vec(10.03f, 21.7f)), module, Droplets::CHORD_PARAM));
		addParam(createParamCentered<Reversed<RoundBlackKnob>>(mm2px(Vec(10.04f, 40.06f)), module, Droplets::RANGE_PARAM));
		addParam(createParamCentered<Reversed<RoundBlackKnob>>(mm2px(Vec(10.04f, 58.42f)), module, Droplets::ROOT_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, Droplets::OCT_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, Droplets::ENV_LIGHT));

		// The faceplate prints Out bottom-left and CV bottom-right, the same way
		// round as every other Mod2 panel; the sketch header's ASCII diagram has
		// that pair drawn the other way and the board is the one to believe.
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, Droplets::TRIG_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.3f)), module, Droplets::ACCENT_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, Droplets::AUDIO_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, Droplets::CV_INPUT));
	}

	void appendContextMenu(Menu* menu) override {
		appendMod2PanelMenu(menu, module);
	}
};

Model* modelDroplets = createModel<Droplets, DropletsWidget>("mod2-droplets");
