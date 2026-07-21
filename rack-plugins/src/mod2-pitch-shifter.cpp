// WIP: Claude-generated maddie synths original (no upstream Hagiwo/Rob
// Scape firmware). Excluded from the VCV Rack build -- see WIP_SOURCES in
// the Makefile and the WIP block in plugin.cpp for how to re-enable it.
#include "plugin.hpp"
#include <PitchShifterCore.h>  // Shared pitch-shift DSP (also used by mod2-pitch-shifter firmware)

/*
	Pitch Shifter — octave & granular shift (delay-line harmonizer).

	Port of firmwares/mod2-pitch-shifter/mod2-pitch-shifter.ino (HAGIWO Mod2, RP2350).

	Mirrors the Mod2 hardware: 3 pots, 1 push button, 1 PWM LED, and the Mod2
	jack set (IN1, IN2, CV, OUT). For the Pitch Shifter firmware:
		POT1 (A0) -> Pitch (+/-12 st, semitone-detented in Free mode)
		POT2 (A1) -> Grain size (10 - 100 ms)
		POT3 (A2) -> unavailable on hardware (pin doubles as the audio input)
		BUTTON    -> mode: Octave-up / Octave-down / Free / Detune
		LED       -> pulses faster the further from unison
		IN1       -> (spare on hardware; no port here)
		IN2       -> octave-down latch gate (>1 V = force -1 oct)
		CV        -> AUDIO INPUT (sampled at the audio rate on hardware)
		OUT       -> audio output

	Deviations from hardware: the firmware's wet/dry and feedback live on shift
	layers (hold BUTTON + turn POT1 / POT2) because POT3's pin is the audio
	input; here the physically-present-but-dead third knob becomes a proper Mix
	control, and feedback (the "shimmer-lite" cascades) is a context-menu
	slider (same pattern as mod2-granular's grain pitch). The delay arena is
	sized for Rack's engine rate. Mode persists in the patch (firmware: flash).
*/

struct PitchShifter : Module {
	enum ParamId {
		PITCH_PARAM,
		GRAIN_PARAM,
		MIX_PARAM,
		MODE_PARAM,   // momentary button — cycles Oct-up / Oct-down / Free / Detune
		FEEDBACK_PARAM,  // shimmer cascades — context-menu slider (firmware BUTTON+POT2)
		PARAMS_LEN
	};
	enum InputId {
		OCTDOWN_INPUT, // IN2 — octave-down latch (>1 V = force -1 oct)
		AUDIO_INPUT,   // CV jack — audio in
		INPUTS_LEN
	};
	enum OutputId {
		AUDIO_OUTPUT,  // OUT
		OUTPUTS_LEN
	};
	enum LightId {
		SHIFT_LIGHT,
		LIGHTS_LEN
	};

	// Pitch-shift state lives in the shared core (same DSP as the firmware).
	sc::PitchShifterCore core;
	std::vector<int16_t> arena;

	dsp::BooleanTrigger modeButton;

	PitchShifter() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(PITCH_PARAM, 0.f, 1.f, 0.5f, "Pitch (±12 st, semitone-detented)");
		configParam(GRAIN_PARAM, 0.f, 1.f, 0.5f, "Grain size (10 → 100 ms)");
		configParam(MIX_PARAM, 0.f, 1.f, 1.f, "Wet/dry mix", "%", 0.f, 100.f);
		configButton(MODE_PARAM, "Mode (Oct-up / Oct-down / Free / Detune)");
		configParam(FEEDBACK_PARAM, 0.f, 1.f, 0.f, "Feedback (shimmer cascades)", "%", 0.f, 100.f);

		configInput(OCTDOWN_INPUT, "IN2 octave-down latch (>1 V = -1 oct)");
		configInput(AUDIO_INPUT, "Audio");
		configOutput(AUDIO_OUTPUT, "Audio");

		configBypass(AUDIO_INPUT, AUDIO_OUTPUT);
		updateArena(APP->engine->getSampleRate());
	}

	// The int16 arena mirrors the firmware's SRAM buffer, sized for the engine
	// rate so the grain excursion is identical at any sample rate.
	void updateArena(float fs) {
		const uint32_t n = sc::pitchShifterArenaSamples(fs);
		if (arena.size() != n) {
			arena.assign(n, 0);
			core.init(arena.data(), n);
		}
	}

	void onSampleRateChange(const SampleRateChangeEvent& e) override {
		updateArena(e.sampleRate);
	}

	void onReset() override {
		core.reset();
		core.mode = sc::PITCH_FREE;
	}

	// Mode persists with the patch (the firmware stores it in flash); feedback
	// is a real Param so Rack persists it on its own.
	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "pitchMode", json_integer(core.mode));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* modeJ = json_object_get(rootJ, "pitchMode");
		if (modeJ)
			core.mode = (uint8_t)clamp((int)json_integer_value(modeJ), 0,
			                           sc::PITCH_MODE_COUNT - 1);
	}

	void process(const ProcessArgs& args) override {
		if (modeButton.process(params[MODE_PARAM].getValue() > 0.5f))
			core.mode = (core.mode + 1) % sc::PITCH_MODE_COUNT;

		// IN2 latch forces octave-down while high (firmware behaviour); the
		// panel mode drives it otherwise.
		const uint8_t panelMode = core.mode;
		const bool octDown = inputs[OCTDOWN_INPUT].getVoltage() > 1.f;

		// Same pot mappings as the firmware (Free mode is semitone-detented).
		core.semitones = sc::pitchShifterSemitones(params[PITCH_PARAM].getValue(), true);
		core.grainSec = sc::pitchShifterGrainSec(params[GRAIN_PARAM].getValue());
		core.wet = params[MIX_PARAM].getValue();
		core.feedback = params[FEEDBACK_PARAM].getValue();
		if (octDown)
			core.mode = sc::PITCH_OCT_DOWN;

		// ±5 V -> -1..1 through the shared core and back.
		const float in = inputs[AUDIO_INPUT].getVoltage() / 5.f;
		float out = core.process(in, args.sampleTime);

		core.mode = panelMode;  // restore panel selection after the latch

		outputs[AUDIO_OUTPUT].setVoltage(clamp(out, -1.f, 1.f) * 5.f);

		// LED pulses with the shift size, as on hardware.
		lights[SHIFT_LIGHT].setBrightness(core.ledLevel());
	}
};

struct PitchShifterWidget : ModuleWidget {
	PitchShifterWidget(PitchShifter* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/mod2-pitch-shifter.svg")));
		// 4 HP Mod1/Mod2 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.03f, 21.7f)), module, PitchShifter::PITCH_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 40.06f)), module, PitchShifter::GRAIN_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 58.42f)), module, PitchShifter::MIX_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, PitchShifter::MODE_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, PitchShifter::SHIFT_LIGHT));

		// IN1 is spare on this firmware, so its jack position stays empty.
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.3f)), module, PitchShifter::OCTDOWN_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, PitchShifter::AUDIO_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, PitchShifter::AUDIO_INPUT));
	}

	// Feedback is the firmware's second shift layer (BUTTON + POT2); the 4 HP
	// panel has no spare knob, so it lives here (same as mod2-granular's pitch).
	void appendContextMenu(Menu* menu) override {
		PitchShifter* module = getModule<PitchShifter>();
		menu->addChild(new MenuSeparator);
		ui::Slider* slider = new ui::Slider;
		slider->quantity = module->paramQuantities[PitchShifter::FEEDBACK_PARAM];
		slider->box.size.x = 200.f;
		menu->addChild(slider);
	}
};

Model* modelPitchShifter = createModel<PitchShifter, PitchShifterWidget>("mod2-pitch-shifter");
