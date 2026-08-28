#include "plugin.hpp"
#include <ChordalCore.h>  // Shared Chordal engine (also used by the firmware)

/*
	Chordal — four-note chording oscillator, sine cross-faded against a square,
	saw or triangle.

	Port of firmwares/mod2-chordal/mod2-chordal.ino (HAGIWO Mod2, RP2350), which
	is itself a port of Sean Luke's GRAINS `chordal`. Synthesis lives in
	sc::ChordalVoice; this file owns only the Rack I/O.

	Mirrors the Mod2 hardware — 3 pots, 1 button, 1 LED, and the Mod2 jack set:
		POT1   -> Chord select (24 voicings, snapped)
		POT2   -> Mix (0 = all sine, 1 = all square/saw/tri)
		POT3   -> Root pitch, C0 up ~5 octaves (the pot shares the CV jack's
		          ADC on hardware, so it doubles as upstream's Pitch Tune)
		BUTTON -> Second waveform: Square -> Saw -> Triangle
		LED    -> Wave-mode indicator (1/3, 2/3, full)
		IN1    -> Trigger: steps the inversion 0..7, wrapping
		IN2    -> unused (the hardware jack exists; this firmware ignores it)
		CV     -> 1 V/oct root pitch, summed with POT3
		OUT    -> audio output

	Divergences from the firmware (as opposed to from GRAINS — the sketch header
	lists those):

	- Inversion has no knob. The hardware hides it behind "hold the button and
	  turn POT1", a gesture a mouse cannot make, so here it is a right-click menu
	  choice (and still steppable from IN1). It saves with the patch.
	- The wave mode is in the right-click menu as well as on the button, since a
	  latched menu choice is easier than clicking a momentary button twice.
	- The firmware's chord-select debounce (the pot must read the same chord for
	  ~78 ms before it commits) is not reproduced: it exists to reject ADC noise
	  at a chord boundary, and a Rack knob has none.
	- CV is standard non-inverted 1 V/oct. The hardware's A2 reads backwards, so
	  the firmware inverts it; converging here means Chordal tracks sequencers
	  the way every other Rack module does.
	- IN2 has no port. The Mod2 board gives every module the same four jacks and
	  Chordal has nothing to do with that one, so the faceplate leaves its label
	  blank and this panel leaves the hole empty; a socket that ignored whatever
	  you patched into it would be worse than no socket.

	License:
	Apache License 2.0 for the engine: the original Chordal firmware is
	Copyright 2023 Sean Luke (sean@cs.gmu.edu), from the GRAINS project
	(github.com/eclab/grains), Apache 2.0. Apache requires the license notice to
	travel with the code and modified files to carry prominent notice of changes
	— the notice lives at firmwares/mod2-chordal/LICENSE.md, and the "Deliberate
	changes from the original" list in the sketch header is the notice of
	changes. Keep both.

	The Rack-side work in this file is MIT License, Copyright (c) 2026 Madelyn
	Yeary, see rack-plugins/LICENSE.md.
*/

// Chord names in the order of sc::chordalChord() — Sean Luke's table order.
static const std::vector<std::string> kChordNames = {
	"None", "m3", "M3", "4", "5", "m6", "M6", "m7",
	"Octave", "Octave + m3", "Octave + M3", "Octave + 5",
	"Minor", "Minor 1st inv", "Minor 2nd inv",
	"Major", "Major 1st inv", "Major 2nd inv",
	"7", "Minor 7", "Major 7", "Diminished 7",
	"Minor + octave", "Major + octave"};

static const std::vector<std::string> kWaveNames = {"Square", "Saw", "Triangle"};

struct Chordal : Mod2Module {
	enum ParamId { CHORD_PARAM, MIX_PARAM, PITCH_PARAM, WAVE_PARAM, PARAMS_LEN };
	enum InputId { INVERSION_INPUT, IN2_INPUT, CV_INPUT, INPUTS_LEN };
	enum OutputId { AUDIO_OUTPUT, OUTPUTS_LEN };
	enum LightId { WAVE_LIGHT, LIGHTS_LEN };

	// Root-pitch range — matches the firmware's A2 mapping (C0 up ~5 octaves).
	static constexpr float ROOT_MIN_HZ = 32.703f;
	static constexpr float VOCT_SPAN = 8.3f * (33.0f / 55.0f);

	sc::ChordalVoice voice;
	dsp::SchmittTrigger inversionTrigger;
	dsp::BooleanTrigger waveButton;

	// Held outside the param system because the hardware has no fourth knob:
	// the firmware keeps both of these in flash, so they persist with the patch.
	int waveMode = sc::kChordalSquare;
	int inversion = 0;

	Chordal() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		// Defaults put the module in a musical place on drop-in (a major triad,
		// half-mixed, two octaves up); the hardware just powers on wherever its
		// pots were left, so there is nothing upstream to match here.
		configSwitch(CHORD_PARAM, 0.f, sc::kChordalNumChords - 1, 15.f, "Chord", kChordNames);
		getParamQuantity(CHORD_PARAM)->snapEnabled = true;
		configParam(MIX_PARAM, 0.f, 1.f, 0.5f, "Mix (sine to square/saw/tri)", "%", 0.f, 100.f);
		configParam(PITCH_PARAM, 0.f, 1.f, 0.4f, "Root pitch (C0 up ~5 octaves)");
		configButton(WAVE_PARAM, "Second waveform (Square/Saw/Triangle)");

		configInput(INVERSION_INPUT, "IN1 trigger (step inversion)");
		configInput(IN2_INPUT, "IN2 (unused)");
		configInput(CV_INPUT, "1V/Oct root pitch");
		configOutput(AUDIO_OUTPUT, "Audio");
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "waveMode", json_integer(waveMode));
		json_object_set_new(root, "inversion", json_integer(inversion));
		mod2WritePanelStyle(root, panelStyle);
		return root;
	}
	void dataFromJson(json_t* root) override {
		if (json_t* w = json_object_get(root, "waveMode"))
			waveMode = clamp((int) json_integer_value(w), 0, sc::kChordalWaveCount - 1);
		if (json_t* i = json_object_get(root, "inversion"))
			inversion = clamp((int) json_integer_value(i), 0, sc::kChordalNumInversions - 1);
		mod2ReadPanelStyle(root, panelStyle);
	}

	void onReset() override {
		voice.reset();
		waveMode = sc::kChordalSquare;
		inversion = 0;
	}

	void process(const ProcessArgs& args) override {
		// Button cycles the second waveform (firmware: short press).
		if (waveButton.process(params[WAVE_PARAM].getValue() > 0.5f))
			waveMode = (waveMode + 1) % sc::kChordalWaveCount;

		// IN1 steps the inversion, wrapping at 8 (firmware: same jack).
		if (inversionTrigger.process(inputs[INVERSION_INPUT].getVoltage(), 0.1f, 1.f))
			inversion = (inversion + 1) % sc::kChordalNumInversions;

		// Root pitch: knob gives C0 up ~5 octaves, CV adds true 1 V/oct.
		const float oct = params[PITCH_PARAM].getValue() * VOCT_SPAN
			+ inputs[CV_INPUT].getVoltage();
		voice.rootFreq = clamp(ROOT_MIN_HZ * std::pow(2.0f, oct), 1.f, 20000.f);

		voice.chord = (uint8_t) clamp((int) std::round(params[CHORD_PARAM].getValue()),
			0, sc::kChordalNumChords - 1);
		voice.mix = params[MIX_PARAM].getValue();
		voice.inversion = (uint8_t) inversion;
		voice.wave = (uint8_t) waveMode;

		// One sample from the shared core: audio in -1..+1 -> ±5 V (Vpp 10 V).
		outputs[AUDIO_OUTPUT].setVoltage(voice.process(args.sampleTime) * 5.f);

		// LED tracks the wave mode, like the firmware (1/3, 2/3, full).
		lights[WAVE_LIGHT].setBrightness((waveMode + 1) / (float) sc::kChordalWaveCount);
	}
};

struct ChordalWidget : ModuleWidget {
	ChordalWidget(Chordal* module) {
		setModule(module);
		setMod2Panel(this, module, "res/mod2-chordal.svg");
		// 4 HP Mod1/Mod2 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<Reversed<RoundBlackKnob>>(mm2px(Vec(10.03f, 21.7f)), module, Chordal::CHORD_PARAM));
		addParam(createParamCentered<Reversed<RoundBlackKnob>>(mm2px(Vec(10.04f, 40.06f)), module, Chordal::MIX_PARAM));
		addParam(createParamCentered<Reversed<RoundBlackKnob>>(mm2px(Vec(10.04f, 58.42f)), module, Chordal::PITCH_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, Chordal::WAVE_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, Chordal::WAVE_LIGHT));

		// IN2 gets no port: the firmware ignores that jack, and the faceplate
		// leaves its label blank, so the hole stays empty rather than offering a
		// socket that does nothing.  The enum entry stays put so CV_INPUT keeps
		// its index and existing patches still load.
		// Out is bottom-left and CV bottom-right, the same way round as every
		// other Mod2 panel; the sketch header's ASCII diagram has that pair drawn
		// the other way and the board is the one to believe.
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, Chordal::INVERSION_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, Chordal::AUDIO_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, Chordal::CV_INPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Chordal* m = dynamic_cast<Chordal*>(module);
		if (m) {
			// Both of these are flash-saved settings on the hardware; the
			// inversion one is the only way to reach the firmware's
			// hold-button-and-turn-POT1 gesture with a mouse.
			menu->addChild(new MenuSeparator);
			menu->addChild(createIndexPtrSubmenuItem("Second waveform", kWaveNames, &m->waveMode));
			menu->addChild(createIndexPtrSubmenuItem("Inversion",
				{"0", "1", "2", "3", "4", "5", "6", "7"}, &m->inversion));
		}
		appendMod2PanelMenu(menu, module);
	}
};

Model* modelChordal = createModel<Chordal, ChordalWidget>("mod2-chordal");
