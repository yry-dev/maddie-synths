// WIP: Claude-generated maddie synths original (no upstream Hagiwo/Rob
// Scape firmware). Excluded from the VCV Rack build -- see WIP_SOURCES in
// the Makefile and the WIP block in plugin.cpp for how to re-enable it.
#include "plugin.hpp"
#include <ReverbCore.h>  // Shared reverb DSP (also used by mod2-reverb firmware)

/*
	Reverb — hall & plate algorithmic reverb.

	Port of firmwares/mod2-reverb/mod2-reverb.ino (HAGIWO Mod2, RP2350).

	Mirrors the Mod2 hardware: 3 pots, 1 push button, 1 PWM LED, and the Mod2
	jack set (IN1, IN2, CV, OUT). For the Reverb firmware:
		POT1 (A0) -> Size (delay-line scaling + Hall pre-delay)
		POT2 (A1) -> Decay (feedback gain; top of range ≈ infinite)
		POT3 (A2) -> unavailable on hardware (pin doubles as the audio input)
		BUTTON    -> mode: Hall / Plate
		LED       -> tail level (pulses slow in Hall, fast in Plate on hardware)
		IN1       -> (spare on hardware; no port here)
		IN2       -> freeze gate (>1 V = infinite tail)
		CV        -> AUDIO INPUT (sampled at the audio rate on hardware)
		OUT       -> audio output

	Deviations from hardware: the firmware's wet/dry lives on a shift layer
	(hold BUTTON + turn POT1) because POT3's pin is the audio input; here the
	physically-present-but-dead third knob becomes a proper Mix control. The
	firmware's other shift layer (hold BUTTON + turn POT2 = damping/tone) and
	its long-press freeze latch have no spare knob/gesture on the panel, so
	Damping is exposed on the right-click context menu and freeze is the IN2
	gate. The tank arena is sized for Rack's engine rate. Mode persists in the
	patch (firmware: flash).
*/

struct Reverb : Module {
	enum ParamId {
		SIZE_PARAM,
		DECAY_PARAM,
		MIX_PARAM,
		DAMP_PARAM,   // context-menu only (firmware: BUTTON + POT2 shift layer)
		MODE_PARAM,   // momentary button — toggles Hall / Plate
		PARAMS_LEN
	};
	enum InputId {
		FREEZE_INPUT, // IN2 — freeze gate (>1 V = infinite tail)
		AUDIO_INPUT,  // CV jack — audio in
		INPUTS_LEN
	};
	enum OutputId {
		AUDIO_OUTPUT, // OUT
		OUTPUTS_LEN
	};
	enum LightId {
		TAIL_LIGHT,
		LIGHTS_LEN
	};

	// Reverb state lives in the shared core (same DSP as the firmware).
	sc::ReverbCore core;
	std::vector<int16_t> arena;

	dsp::BooleanTrigger modeButton;

	Reverb() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(SIZE_PARAM, 0.f, 1.f, 0.6f, "Size (delay scaling + pre-delay)", "%", 0.f, 100.f);
		configParam(DECAY_PARAM, 0.f, 1.f, 0.6f, "Decay (feedback; top ≈ infinite)", "%", 0.f, 100.f);
		configParam(MIX_PARAM, 0.f, 1.f, 0.4f, "Wet/dry mix", "%", 0.f, 100.f);
		configParam(DAMP_PARAM, 0.f, 1.f, 0.5f, "Damping / tone", "%", 0.f, 100.f);
		configButton(MODE_PARAM, "Mode (Hall / Plate)");

		configInput(FREEZE_INPUT, "IN2 freeze gate (>1 V = infinite tail)");
		configInput(AUDIO_INPUT, "Audio");
		configOutput(AUDIO_OUTPUT, "Audio");

		configBypass(AUDIO_INPUT, AUDIO_OUTPUT);
		updateArena(APP->engine->getSampleRate());
	}

	// The int16 tank arena mirrors the firmware's SRAM buffer, sized for the
	// engine rate so the delay times are identical at any sample rate.
	void updateArena(float fs) {
		const uint32_t n = sc::reverbArenaSamples(fs);
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
		core.mode = sc::REVERB_HALL;
	}

	// The mode persists with the patch (the firmware stores it in flash).
	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "reverbMode", json_integer(core.mode));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* modeJ = json_object_get(rootJ, "reverbMode");
		if (modeJ)
			core.mode = (uint8_t)clamp((int)json_integer_value(modeJ), 0,
			                           sc::REVERB_MODE_COUNT - 1);
	}

	void process(const ProcessArgs& args) override {
		if (modeButton.process(params[MODE_PARAM].getValue() > 0.5f))
			core.mode = (core.mode + 1) % sc::REVERB_MODE_COUNT;

		// Same pot mappings as the firmware.
		core.sizeScale = sc::reverbSizeScale(params[SIZE_PARAM].getValue());
		core.decayGain = sc::reverbDecayGain(params[DECAY_PARAM].getValue());
		core.damping = sc::reverbDampingAmount(params[DAMP_PARAM].getValue());
		core.wet = params[MIX_PARAM].getValue();

		// IN2 freeze gate: >1 V holds the tail (infinite), input muted.
		core.freeze = inputs[FREEZE_INPUT].getVoltage() > 1.f;

		// +/-5 V -> -1..1 through the shared core and back.
		const float in = inputs[AUDIO_INPUT].getVoltage() / 5.f;
		const float out = core.process(in, args.sampleTime);
		outputs[AUDIO_OUTPUT].setVoltage(clamp(out, -1.5f, 1.5f) * 5.f);

		// LED follows the tail level, as on hardware.
		lights[TAIL_LIGHT].setBrightness(core.ledLevel());
	}
};

struct ReverbWidget : ModuleWidget {
	ReverbWidget(Reverb* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/mod2-reverb.svg")));
		// 4 HP Mod1/Mod2 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.03f, 21.7f)), module, Reverb::SIZE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 40.06f)), module, Reverb::DECAY_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 58.42f)), module, Reverb::MIX_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, Reverb::MODE_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, Reverb::TAIL_LIGHT));

		// IN1 is spare on this firmware, so its jack position stays empty.
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.3f)), module, Reverb::FREEZE_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, Reverb::AUDIO_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, Reverb::AUDIO_INPUT));
	}

	// Damping is a firmware shift-layer control with no spare knob on the panel,
	// so it lives on the right-click menu here.
	void appendContextMenu(Menu* menu) override {
		Reverb* module = getModule<Reverb>();
		if (!module)
			return;
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Reverb"));
		menu->addChild(new ParamSlider(module->paramQuantities[Reverb::DAMP_PARAM]));
	}

	// Minimal labelled slider wrapping a ParamQuantity (Damping/tone).
	struct ParamSlider : ui::Slider {
		ParamSlider(engine::ParamQuantity* pq) {
			quantity = pq;
			box.size.x = 200.f;
		}
	};
};

Model* modelReverb = createModel<Reverb, ReverbWidget>("mod2-reverb");
