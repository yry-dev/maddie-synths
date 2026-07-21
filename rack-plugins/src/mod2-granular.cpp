// WIP: Claude-generated maddie synths original (no upstream Hagiwo/Rob
// Scape firmware). Excluded from the VCV Rack build -- see WIP_SOURCES in
// the Makefile and the WIP block in plugin.cpp for how to re-enable it.
#include "plugin.hpp"
#include <GranularCore.h>  // Shared granular DSP (also used by mod2-granular firmware)

/*
	Granular — live-buffer granular delay / cloud textures.

	Port of firmwares/mod2-granular/mod2-granular.ino (HAGIWO Mod2, RP2350).

	Mirrors the Mod2 hardware: 3 pots, 1 push button, 1 PWM LED, and the Mod2
	jack set (IN1, IN2, CV, OUT). For the Granular firmware:
		POT1 (A0) -> Grain size (10 - 250 ms, exponential taper)
		POT2 (A1) -> Texture macro (grain density + spray + pitch jitter)
		POT3 (A2) -> unavailable on hardware (pin doubles as the audio input)
		BUTTON    -> character: Smooth (Hann) / Perc (expodec) / Reverse
		LED       -> flickers on each grain spawn (density is visible)
		IN1       -> grain trigger (patch a clock for rhythmic granular)
		IN2       -> freeze gate (>1 V = stop recording, granulate held buffer)
		CV        -> AUDIO INPUT (sampled at the audio rate on hardware)
		OUT       -> audio output

	Deviations from hardware: the firmware's two shift layers live on
	physically-present controls here. Wet/dry (hold BUTTON + POT1) becomes the
	dead third knob's proper Mix control; grain pitch (hold BUTTON + POT2), which
	has no spare knob on the 4 HP panel, lives in the right-click context menu
	(same approach as mod2-comb's damping). The record buffer is sized for Rack's
	engine rate. The grain character persists in the patch (firmware: flash).
*/

struct Granular : Module {
	enum ParamId {
		SIZE_PARAM,
		DENSITY_PARAM,
		MIX_PARAM,
		PITCH_PARAM,  // grain pitch — context-menu slider (firmware BUTTON+POT2)
		MODE_PARAM,   // momentary button — cycles Smooth / Perc / Reverse
		PARAMS_LEN
	};
	enum InputId {
		TRIG_INPUT,   // IN1 — grain trigger
		FREEZE_INPUT, // IN2 — freeze gate (>1 V = stop recording)
		AUDIO_INPUT,  // CV jack — audio in
		INPUTS_LEN
	};
	enum OutputId {
		AUDIO_OUTPUT, // OUT
		OUTPUTS_LEN
	};
	enum LightId {
		GRAIN_LIGHT,
		LIGHTS_LEN
	};

	// Granular state lives in the shared core (same DSP as the firmware).
	sc::GranularCore core;
	std::vector<int16_t> arena;

	dsp::BooleanTrigger modeButton;
	dsp::SchmittTrigger grainTrigger;  // IN1 external grain clock

	Granular() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(SIZE_PARAM, 0.f, 1.f, 0.35f, "Grain size (10 → 250 ms)");
		configParam(DENSITY_PARAM, 0.f, 1.f, 0.5f, "Texture (density + spray + jitter)", "%", 0.f, 100.f);
		configParam(MIX_PARAM, 0.f, 1.f, 0.5f, "Wet/dry mix", "%", 0.f, 100.f);
		configParam(PITCH_PARAM, 0.f, 1.f, 0.5f, "Grain pitch (−12 / −7 / 0 / +7 / +12 st)");
		configButton(MODE_PARAM, "Character (Smooth / Perc / Reverse)");

		configInput(TRIG_INPUT, "IN1 grain trigger (rising edge spawns a grain)");
		configInput(FREEZE_INPUT, "IN2 freeze gate (>1 V = stop recording)");
		configInput(AUDIO_INPUT, "Audio");
		configOutput(AUDIO_OUTPUT, "Audio");

		configBypass(AUDIO_INPUT, AUDIO_OUTPUT);
		updateArena(APP->engine->getSampleRate());
	}

	// The int16 record buffer mirrors the firmware's SRAM arena, sized for the
	// engine rate so grain excursions are identical at any sample rate.
	void updateArena(float fs) {
		const uint32_t n = sc::granularArenaSamples(fs);
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
		core.mode = sc::GRAN_SMOOTH;
	}

	// The grain character persists with the patch (the firmware stores it in
	// flash). Grain pitch is a Param, so Rack persists it automatically.
	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "granMode", json_integer(core.mode));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* modeJ = json_object_get(rootJ, "granMode");
		if (modeJ)
			core.mode = (uint8_t)clamp((int)json_integer_value(modeJ), 0,
			                           sc::GRAN_MODE_COUNT - 1);
	}

	void process(const ProcessArgs& args) override {
		if (modeButton.process(params[MODE_PARAM].getValue() > 0.5f))
			core.mode = (core.mode + 1) % sc::GRAN_MODE_COUNT;

		// Same pot mappings as the firmware.
		core.sizeSec = sc::granularSizeSec(params[SIZE_PARAM].getValue());
		core.density = params[DENSITY_PARAM].getValue();
		core.pitchSemi = sc::granularPitchSemi(params[PITCH_PARAM].getValue());
		core.mix = params[MIX_PARAM].getValue();

		// IN1 rising edge spawns a grain (external-clock granular).
		if (grainTrigger.process(inputs[TRIG_INPUT].getVoltage(), 0.1f, 1.f))
			core.triggerGrain();

		// IN2 freeze gate: >1 V stops recording, granulates the held buffer.
		core.freeze = inputs[FREEZE_INPUT].getVoltage() > 1.f;

		// +/-5 V -> -1..1 through the shared core and back.
		const float in = inputs[AUDIO_INPUT].getVoltage() / 5.f;
		const float out = core.process(in, args.sampleTime);
		outputs[AUDIO_OUTPUT].setVoltage(clamp(out, -1.f, 1.f) * 5.f);

		// LED flickers on each grain spawn, as on hardware.
		lights[GRAIN_LIGHT].setBrightness(core.ledLevel());
	}
};

struct GranularWidget : ModuleWidget {
	GranularWidget(Granular* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/mod2-granular.svg")));
		// 4 HP Mod1/Mod2 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.03f, 21.7f)), module, Granular::SIZE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 40.06f)), module, Granular::DENSITY_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 58.42f)), module, Granular::MIX_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, Granular::MODE_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, Granular::GRAIN_LIGHT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, Granular::TRIG_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.3f)), module, Granular::FREEZE_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, Granular::AUDIO_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, Granular::AUDIO_INPUT));
	}

	// Grain pitch is the firmware's second shift layer (BUTTON + POT2); the 4 HP
	// panel has no spare knob, so it lives here (same as mod2-comb's damping).
	void appendContextMenu(Menu* menu) override {
		Granular* module = getModule<Granular>();
		menu->addChild(new MenuSeparator);
		ui::Slider* slider = new ui::Slider;
		slider->quantity = module->paramQuantities[Granular::PITCH_PARAM];
		slider->box.size.x = 200.f;
		menu->addChild(slider);
	}
};

Model* modelGranular = createModel<Granular, GranularWidget>("mod2-granular");
