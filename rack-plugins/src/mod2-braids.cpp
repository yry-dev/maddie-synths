#include "plugin.hpp"

/*
	Tiny Braids — Mutable Instruments Braids macro-oscillator: 48 synthesis
	engines behind one Timbre / Morph control pair.

	Port of firmwares/mod2-braids/mod2-braids.ino (HAGIWO Mod2, RP2350).

	Mirrors the Mod2 hardware: 3 pots, 1 push button, 1 LED, and the Mod2
	jack set. For the Tiny Braids firmware:
		POT1    -> Timbre (engine timbre, 0-32767)
		POT2    -> Morph / colour (0-32767)
		POT3    -> Pitch (MIDI notes 24-64, shared with CV on hardware)
		BUTTON  -> engine select (short = next engine)
		LED     -> follows the gate / trigger
		TRIGGER -> trigger / gate input (fires a note)
		IN2     -> reserved (unused, like the hardware)
		CV      -> 1V/Oct pitch
		OUT     -> audio output

	Divergences from the firmware:
	 - Hardware shares A2 between POT3 and the CV jack; here the knob sets a
	   base pitch (MIDI 24-64) and the jack is standard 1V/Oct on top.
	 - The hardware long-press (previous engine) doesn't map well to a Rack
	   button, so the button steps forward and the context menu lists all 48
	   engines by name (including the Question Mark easter egg the hardware
	   button skips).
	 - The firmware imposes a simple attack/release envelope on continuous
	   engines, which makes them silent until triggered. That behaviour is
	   kept when the Trigger input is patched, but an unpatched Trigger lets
	   continuous engines free-run at full level so the module drones out of
	   the box. Percussive engines still need a trigger, like real Braids.

	Build note: depends on the EXTERNAL Mutable Instruments BRAIDS + STMLIB
	libraries (poetaster/arduinoMI packaging), deliberately not vendored in
	this repo — see README.md "MOD2 Braids / Tides" for the install steps.
	The Makefile picks them up via MI_LIB_DIR (default: the Arduino libraries
	folder); CI fetches them at the commits pinned in .github/mi-libs.env.
	Library bodies are compiled once in src/mi-libs.cpp.

	License:
	MIT License, Copyright (c) 2026 Madelyn Yeary — see rack-plugins/LICENSE.md.
	Wraps the Mutable Instruments Braids DSP, Copyright (c) Émilie Gillet (MIT
	License), compiled from the external poetaster/arduinoMI packaging rather
	than vendored here. (The firmware wrapper this mirrors is GPLv3, (c) 2025
	blueprint@poetaster.de; this file calls the MIT-licensed MI sources
	directly and shares no code with it.)
*/

#if !__has_include(<braids/macro_oscillator.h>)
#error "mod2-braids needs the external Mutable Instruments BRAIDS/STMLIB libraries (poetaster/arduinoMI) — see README.md 'MOD2 Braids / Tides'. Install them (MI_LIB_DIR in rack-plugins/Makefile), or exclude this module via WIP_SOURCES + plugin.{hpp,cpp} + the root plugin.json."
#endif

// TEST: Mutable's desktop-build switch — see src/mi-libs.cpp.
#define TEST
#include <STMLIB.h>
#include <BRAIDS.h>
// The library bodies (lookup tables + out-of-line definitions) are compiled
// exactly once, in src/mi-libs.cpp — this file uses only the headers.

struct TinyBraids : Mod2Module {
	enum ParamId {
		TIMBRE_PARAM,
		MORPH_PARAM,
		PITCH_PARAM,
		ENGINE_PARAM,  // momentary button: next engine
		PARAMS_LEN
	};
	enum InputId {
		TRIG_INPUT,   // trigger / gate (fires a note)
		IN2_INPUT,    // reserved (unused, like the hardware)
		CV_INPUT,     // 1V/Oct pitch
		INPUTS_LEN
	};
	enum OutputId {
		AUDIO_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		GATE_LIGHT,
		LIGHTS_LEN
	};

	// Pitch knob range — matches the firmware POT3 mapping (pitch units
	// 3072..8192 = MIDI notes 24..64).
	static constexpr float PITCH_MIN_NOTE = 24.0f;
	static constexpr float PITCH_RANGE    = 40.0f;

	// Same block size as the firmware (braids.h BLOCK_SIZE).
	static constexpr int BLOCK = 32;

	braids::MacroOscillator osc;
	int16_t buffer[BLOCK] = {};
	uint8_t syncBuffer[BLOCK] = {};
	int blockPos = 0;
	float oscSampleRate = 0.f;  // rate osc was Init()ed for; re-init on change

	int engine = 0;  // MacroOscillatorShape index (firmware boots at 0 = CSAW)
	bool lastGate = false;
	// Firmware's per-block attack/release envelope for continuous engines.
	float envLevel = 0.f;
	float envTarget = 0.f;

	dsp::BooleanTrigger engineButton;

	TinyBraids() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(TIMBRE_PARAM, 0.f, 1.f, 0.5f, "Timbre");
		configParam(MORPH_PARAM, 0.f, 1.f, 0.5f, "Morph / colour");
		configParam(PITCH_PARAM, 0.f, 1.f, 0.5f, "Pitch (MIDI note)", "", 0.f, PITCH_RANGE, PITCH_MIN_NOTE);
		configButton(ENGINE_PARAM, "Next engine");

		configInput(TRIG_INPUT, "Trigger / gate");
		configInput(IN2_INPUT, "IN2 (reserved — unused)");
		configInput(CV_INPUT, "1V/Oct pitch");
		configOutput(AUDIO_OUTPUT, "Audio");
	}

	void initOsc(float sampleRate) {
		// Same init sequence as the firmware: zero the object, then Init()
		// with the actual render rate (poetaster's packaging made Init take
		// the sample rate, so pitch tracks correctly at any Rack rate).
		memset(&osc, 0, sizeof(osc));
		osc.Init(sampleRate);
		oscSampleRate = sampleRate;
	}

	void onReset() override {
		initOsc(oscSampleRate > 0.f ? oscSampleRate : 48000.f);
		engine = 0;
		lastGate = false;
		envLevel = envTarget = 0.f;
		blockPos = 0;
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "engine", json_integer(engine));
		mod2WritePanelStyle(root, panelStyle);
		return root;
	}
	void dataFromJson(json_t* root) override {
		if (json_t* j = json_object_get(root, "engine"))
			engine = clamp((int) json_integer_value(j), 0, (int) braids::MACRO_OSC_SHAPE_LAST - 1);
		mod2ReadPanelStyle(root, panelStyle);
	}

	void renderBlock(const ProcessArgs& args) {
		if (args.sampleRate != oscSampleRate)
			initOsc(args.sampleRate);

		// Pitch: knob MIDI 24-64 + 1V/Oct CV, in Braids' 1/128-semitone units.
		const float note = PITCH_MIN_NOTE + PITCH_RANGE * params[PITCH_PARAM].getValue()
			+ 12.f * inputs[CV_INPUT].getVoltage();
		osc.set_pitch(clamp((int) (note * 128.f), 0, 16383));

		const int shape = clamp(engine, 0, (int) braids::MACRO_OSC_SHAPE_LAST - 1);
		osc.set_shape(static_cast<braids::MacroOscillatorShape>(shape));

		const int16_t timbre = (int16_t) (params[TIMBRE_PARAM].getValue() * 32767.f);
		const int16_t morph = (int16_t) (params[MORPH_PARAM].getValue() * 32767.f);
		osc.set_parameters(timbre, morph);

		// Trigger edge detection (same as the firmware).
		const bool gateConnected = inputs[TRIG_INPUT].isConnected();
		const bool gate = inputs[TRIG_INPUT].getVoltage() >= 1.f;
		const bool triggerFlag = gate && !lastGate;
		const bool triggerRelease = !gate && lastGate;
		lastGate = gate;

		// Engine classes, straight from the firmware's updateBraidsAudio().
		const bool isPercussive =
			shape == braids::MACRO_OSC_SHAPE_PLUCKED ||
			shape == braids::MACRO_OSC_SHAPE_BOWED ||
			shape == braids::MACRO_OSC_SHAPE_BLOWN ||
			shape == braids::MACRO_OSC_SHAPE_FLUTED ||
			shape == braids::MACRO_OSC_SHAPE_STRUCK_BELL ||
			shape == braids::MACRO_OSC_SHAPE_STRUCK_DRUM ||
			shape == braids::MACRO_OSC_SHAPE_KICK ||
			shape == braids::MACRO_OSC_SHAPE_SNARE ||
			shape == braids::MACRO_OSC_SHAPE_CYMBAL ||
			shape == braids::MACRO_OSC_SHAPE_PARTICLE_NOISE ||
			shape == braids::MACRO_OSC_SHAPE_DIGITAL_MODULATION;
		const bool isGated =
			shape == braids::MACRO_OSC_SHAPE_VOWEL ||
			shape == braids::MACRO_OSC_SHAPE_VOWEL_FOF;

		if (isPercussive) {
			if (triggerFlag)
				osc.Strike();
			osc.Render(syncBuffer, buffer, BLOCK);
		}
		else if (isGated) {
			if (triggerFlag)
				osc.Strike();
			memset(syncBuffer, gate ? 0xFF : 0x00, BLOCK);
			osc.Render(syncBuffer, buffer, BLOCK);
		}
		else {
			// Continuous engines. Firmware: an attack/release envelope tied
			// to the trigger (silent until fired). Port divergence: with no
			// cable in Trigger the envelope is bypassed so the oscillator
			// free-runs — see the header comment.
			if (triggerFlag) {
				osc.Strike();  // reset phase
				envTarget = 1.f;
			}
			else if (triggerRelease) {
				envTarget = 0.f;
			}
			// Same per-block rates as the firmware (~67 ms attack, ~670 ms
			// release at 48 kHz).
			if (envTarget > envLevel)
				envLevel = std::min(envLevel + 0.01f, envTarget);
			else
				envLevel = std::max(envLevel - 0.001f, 0.f);

			osc.Render(syncBuffer, buffer, BLOCK);

			if (gateConnected) {
				for (int i = 0; i < BLOCK; i++)
					buffer[i] = (int16_t) (buffer[i] * envLevel);
			}
		}
	}

	void process(const ProcessArgs& args) override {
		// Engine button: step to the next engine, wrapping (firmware short
		// press; long-press-for-previous lives in the context menu instead).
		if (engineButton.process(params[ENGINE_PARAM].getValue() > 0.5f))
			engine = (engine + 1) % (int) braids::MACRO_OSC_SHAPE_LAST;

		if (blockPos == 0)
			renderBlock(args);

		// int16 engine output -> ±5 V.
		outputs[AUDIO_OUTPUT].setVoltage(buffer[blockPos] * (5.f / 32768.f));
		lights[GATE_LIGHT].setBrightnessSmooth(lastGate ? 1.f : 0.f, args.sampleTime);

		blockPos = (blockPos + 1) % BLOCK;
	}
};

// Menu names, in MacroOscillatorShape enum order (see braids/settings.h).
static const std::vector<std::string> tinyBraidsEngineNames = {
	"CSAW", "Morph (saw-square-tri)", "Saw x square", "Sine-triangle", "Buzz",
	"Square sub", "Saw sub", "Square sync", "Saw sync",
	"Triple saw", "Triple square", "Triple triangle", "Triple sine", "Triple ring mod",
	"Saw swarm", "Saw comb", "Toy",
	"Digital filter LP", "Digital filter PK", "Digital filter BP", "Digital filter HP",
	"VOSIM", "Vowel", "Vowel FOF",
	"Harmonics",
	"FM", "Feedback FM", "Chaotic feedback FM",
	"Plucked", "Bowed", "Blown", "Fluted",
	"Struck bell", "Struck drum", "Kick", "Cymbal", "Snare",
	"Wavetables", "Wave map", "Wave line", "Wave paraphonic",
	"Filtered noise", "Twin peaks noise", "Clocked noise",
	"Granular cloud", "Particle noise",
	"Digital modulation", "Question mark",
};

struct TinyBraidsWidget : ModuleWidget {
	TinyBraidsWidget(TinyBraids* module) {
		setModule(module);
		setMod2Panel(this, module, "res/mod2-braids.svg");
		// 4 HP Mod2 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<Reversed<RoundBlackKnob>>(mm2px(Vec(10.03f, 21.70f)), module, TinyBraids::TIMBRE_PARAM));
		addParam(createParamCentered<Reversed<RoundBlackKnob>>(mm2px(Vec(10.04f, 40.06f)), module, TinyBraids::MORPH_PARAM));
		addParam(createParamCentered<Reversed<RoundBlackKnob>>(mm2px(Vec(10.04f, 58.42f)), module, TinyBraids::PITCH_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, TinyBraids::ENGINE_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, TinyBraids::GATE_LIGHT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, TinyBraids::TRIG_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.30f)), module, TinyBraids::IN2_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, TinyBraids::AUDIO_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, TinyBraids::CV_INPUT));
	}

	void appendContextMenu(Menu* menu) override {
		TinyBraids* m = dynamic_cast<TinyBraids*>(module);
		if (!m)
			return;
		menu->addChild(new MenuSeparator);
		menu->addChild(createIndexPtrSubmenuItem("Engine", tinyBraidsEngineNames, &m->engine));
		appendMod2PanelMenu(menu, module);
	}
};

Model* modelTinyBraids = createModel<TinyBraids, TinyBraidsWidget>("mod2-braids");
