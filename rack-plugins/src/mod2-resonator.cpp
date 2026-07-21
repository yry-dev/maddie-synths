// WIP: Claude-generated maddie synths original (no upstream Hagiwo/Rob
// Scape firmware). Excluded from the VCV Rack build -- see WIP_SOURCES in
// the Makefile and the WIP block in plugin.cpp for how to re-enable it.
#include "plugin.hpp"
#include <ResonatorCore.h>  // Shared resonator DSP (also used by mod2-resonator firmware)

/*
	Resonator — Rings-like tuned resonator bank.

	Port of firmwares/mod2-resonator/mod2-resonator.ino (HAGIWO Mod2, RP2350).

	Mirrors the Mod2 hardware: 3 pots, 1 push button, 1 PWM LED, and the Mod2
	jack set (IN1, IN2, CV, OUT). For the Resonator firmware:
		POT1 (A0) -> Pitch / fundamental (semitone-quantized, A1 - A5)
		POT2 (A1) -> Structure (partial spread / string detune spread)
		POT3 (A2) -> unavailable on hardware (pin doubles as the audio input)
		BUTTON    -> mode: Modal / Comb cluster / Sympathetic
		LED       -> follows the bank's energy
		IN1       -> strike trigger (internal noise-burst exciter)
		IN2       -> damp gate (>1 V = choke the bank)
		CV        -> AUDIO INPUT (sampled at the audio rate on hardware)
		OUT       -> audio output

	Deviations from hardware: the firmware's wet/dry lives on a shift layer
	(hold BUTTON + turn POT1) because POT3's pin is the audio input; here the
	physically-present-but-dead third knob becomes a proper Mix control. The
	second shift layer (hold BUTTON + turn POT2 = damping / decay time) has no
	spare knob at all, so Damping lives in the context menu. The button's
	long-press strike is dropped — trigger IN1 instead. The mode persists in
	the patch (firmware: flash).
*/

struct Resonator : Module {
	enum ParamId {
		PITCH_PARAM,
		STRUCT_PARAM,
		MIX_PARAM,
		MODE_PARAM,   // momentary button — cycles Modal / Comb / Sympathetic
		DAMP_PARAM,   // context-menu slider (hardware: BUTTON + POT2)
		PARAMS_LEN
	};
	enum InputId {
		STRIKE_INPUT, // IN1 — strike trigger
		DAMP_INPUT,   // IN2 — damp gate (>1 V = choke)
		AUDIO_INPUT,  // CV jack — audio in
		INPUTS_LEN
	};
	enum OutputId {
		AUDIO_OUTPUT, // OUT
		OUTPUTS_LEN
	};
	enum LightId {
		ENERGY_LIGHT,
		LIGHTS_LEN
	};

	// Resonator state lives in the shared core (same DSP as the firmware).
	sc::ResonatorCore core;
	std::vector<int16_t> arena;

	dsp::SchmittTrigger strikeTrigger;
	dsp::BooleanTrigger modeButton;

	Resonator() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(PITCH_PARAM, 0.f, 1.f, 0.5f, "Pitch (semitone-quantized, A1 → A5)");
		configParam(STRUCT_PARAM, 0.f, 1.f, 0.5f, "Structure (partial / detune spread)", "%", 0.f, 100.f);
		configParam(MIX_PARAM, 0.f, 1.f, 0.5f, "Wet/dry mix", "%", 0.f, 100.f);
		configButton(MODE_PARAM, "Mode (Modal / Comb cluster / Sympathetic)");
		configParam(DAMP_PARAM, 0.f, 1.f, 0.5f, "Damping / decay time", "%", 0.f, 100.f);

		configInput(STRIKE_INPUT, "IN1 strike trigger (noise-burst exciter)");
		configInput(DAMP_INPUT, "IN2 damp gate (>1 V = choke)");
		configInput(AUDIO_INPUT, "Audio (excitation)");
		configOutput(AUDIO_OUTPUT, "Audio");

		configBypass(AUDIO_INPUT, AUDIO_OUTPUT);
		updateArena(APP->engine->getSampleRate());
	}

	// The int16 arena mirrors the firmware's SRAM buffer, sized for the
	// engine rate so the lowest pitch always fits the delay segments.
	void updateArena(float fs) {
		const uint32_t n = sc::resonatorArenaSamples(fs);
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
		core.mode = sc::RESONATOR_MODAL;
	}

	// The mode persists with the patch (the firmware stores it in flash).
	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "resonatorMode", json_integer(core.mode));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* modeJ = json_object_get(rootJ, "resonatorMode");
		if (modeJ)
			core.mode = (uint8_t)clamp((int)json_integer_value(modeJ), 0,
			                           sc::RESONATOR_MODE_COUNT - 1);
	}

	void process(const ProcessArgs& args) override {
		if (modeButton.process(params[MODE_PARAM].getValue() > 0.5f))
			core.mode = (core.mode + 1) % sc::RESONATOR_MODE_COUNT;

		// IN1 strike: rings the bank with the internal noise-burst exciter.
		if (strikeTrigger.process(inputs[STRIKE_INPUT].getVoltage(), 0.1f, 1.f))
			core.strike();

		// Same pot mappings as the firmware.
		core.pitchHz = sc::resonatorPitchHz(params[PITCH_PARAM].getValue());
		core.structure = params[STRUCT_PARAM].getValue();
		core.damping = params[DAMP_PARAM].getValue();
		core.wet = params[MIX_PARAM].getValue();
		core.dampGate = inputs[DAMP_INPUT].getVoltage() > 1.f;

		// +/-5 V -> -1..1 through the shared core and back.
		const float in = inputs[AUDIO_INPUT].getVoltage() / 5.f;
		const float out = core.process(in, args.sampleTime);
		outputs[AUDIO_OUTPUT].setVoltage(clamp(out, -1.f, 1.f) * 5.f);

		// LED follows the bank's energy, as on hardware.
		lights[ENERGY_LIGHT].setBrightness(core.ledLevel());
	}
};

struct ResonatorWidget : ModuleWidget {
	ResonatorWidget(Resonator* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/mod2-resonator.svg")));
		// 4 HP Mod1/Mod2 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.03f, 21.7f)), module, Resonator::PITCH_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 40.06f)), module, Resonator::STRUCT_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 58.42f)), module, Resonator::MIX_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, Resonator::MODE_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, Resonator::ENERGY_LIGHT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, Resonator::STRIKE_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.3f)), module, Resonator::DAMP_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, Resonator::AUDIO_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, Resonator::AUDIO_INPUT));
	}

	// Damping is the firmware's second shift layer (BUTTON + POT2); with no
	// spare knob on the 4 HP panel it lives here in the context menu.
	void appendContextMenu(Menu* menu) override {
		Resonator* module = getModule<Resonator>();
		menu->addChild(new MenuSeparator);
		ui::Slider* slider = new ui::Slider;
		slider->quantity = module->paramQuantities[Resonator::DAMP_PARAM];
		slider->box.size.x = 200.f;
		menu->addChild(slider);
	}
};

Model* modelResonator = createModel<Resonator, ResonatorWidget>("mod2-resonator");
