// WIP: Claude-generated maddie synths original (no upstream Hagiwo/Rob
// Scape firmware). Excluded from the VCV Rack build -- see WIP_SOURCES in
// the Makefile and the WIP block in plugin.cpp for how to re-enable it.
#include "plugin.hpp"
#include <KarplusCore.h>  // Shared karplus DSP (also used by mod2-karplus firmware)

/*
	Karplus — dedicated Karplus-Strong plucked-string voice / processor.

	Port of firmwares/mod2-karplus/mod2-karplus.ino (HAGIWO Mod2, RP2350).

	Mirrors the Mod2 hardware: 3 pots, 1 push button, 1 PWM LED, and the Mod2
	jack set (IN1, IN2, CV, OUT). For the Karplus firmware:
		POT1 (A0) -> Pitch / fundamental (semitone-quantized, A1 - A5)
		POT2 (A1) -> Damping / brightness (loop-filter cutoff + decay)
		POT3 (A2) -> unavailable on hardware (pin doubles as the audio input)
		BUTTON    -> mode: Pluck / Bow / Drone
		LED       -> follows the string's energy (flashes on each pluck)
		IN1       -> pluck trigger (internal noise-burst exciter)
		IN2       -> damp gate (>1 V = palm-mute the string)
		CV        -> AUDIO INPUT (sampled at the audio rate on hardware)
		OUT       -> audio output

	Deviations from hardware: the firmware's wet/dry lives on a shift layer
	(hold BUTTON + turn POT1) because POT3's pin is the audio input; here the
	physically-present-but-dead third knob becomes a proper Mix control. The
	second shift layer (hold BUTTON + turn POT2 = excitation colour) has no
	spare knob, so Colour lives in the context menu. The button's long-press
	pluck is dropped — trigger IN1 instead. The mode persists in the patch
	(firmware: flash).
*/

struct Karplus : Module {
	enum ParamId {
		PITCH_PARAM,
		DAMP_PARAM,
		MIX_PARAM,
		MODE_PARAM,    // momentary button — cycles Pluck / Bow / Drone
		COLOUR_PARAM,  // context-menu slider (hardware: BUTTON + POT2)
		PARAMS_LEN
	};
	enum InputId {
		PLUCK_INPUT,  // IN1 — pluck trigger
		DAMP_INPUT,   // IN2 — damp gate (>1 V = palm-mute)
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

	// Karplus state lives in the shared core (same DSP as the firmware).
	sc::KarplusCore core;
	std::vector<int16_t> arena;

	dsp::SchmittTrigger pluckTrigger;
	dsp::BooleanTrigger modeButton;

	Karplus() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(PITCH_PARAM, 0.f, 1.f, 0.5f, "Pitch (semitone-quantized, A1 → A5)");
		configParam(DAMP_PARAM, 0.f, 1.f, 0.5f, "Damping / decay (bright/long → dark/short)", "%", 0.f, 100.f);
		configParam(MIX_PARAM, 0.f, 1.f, 1.f, "Wet/dry mix (dry = excitation)", "%", 0.f, 100.f);
		configButton(MODE_PARAM, "Mode (Pluck / Bow / Drone)");
		configParam(COLOUR_PARAM, 0.f, 1.f, 0.5f, "Excitation colour (dark → bright)", "%", 0.f, 100.f);

		configInput(PLUCK_INPUT, "IN1 pluck trigger (noise-burst exciter)");
		configInput(DAMP_INPUT, "IN2 damp gate (>1 V = palm-mute)");
		configInput(AUDIO_INPUT, "Audio (excitation)");
		configOutput(AUDIO_OUTPUT, "Audio");

		configBypass(AUDIO_INPUT, AUDIO_OUTPUT);
		updateArena(APP->engine->getSampleRate());
	}

	// The int16 arena mirrors the firmware's SRAM buffer, sized for the engine
	// rate so the lowest pitch always fits the string delay.
	void updateArena(float fs) {
		const uint32_t n = sc::karplusArenaSamples(fs);
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
		core.mode = sc::KARPLUS_PLUCK;
	}

	// The mode persists with the patch (the firmware stores it in flash).
	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "karplusMode", json_integer(core.mode));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* modeJ = json_object_get(rootJ, "karplusMode");
		if (modeJ)
			core.mode = (uint8_t)clamp((int)json_integer_value(modeJ), 0,
			                           sc::KARPLUS_MODE_COUNT - 1);
	}

	void process(const ProcessArgs& args) override {
		if (modeButton.process(params[MODE_PARAM].getValue() > 0.5f))
			core.mode = (core.mode + 1) % sc::KARPLUS_MODE_COUNT;

		// IN1 pluck: rings the string with the internal noise-burst exciter.
		if (pluckTrigger.process(inputs[PLUCK_INPUT].getVoltage(), 0.1f, 1.f))
			core.pluck();

		// Same pot mappings as the firmware.
		core.pitchHz = sc::karplusPitchHz(params[PITCH_PARAM].getValue());
		core.damping = params[DAMP_PARAM].getValue();
		core.colour = params[COLOUR_PARAM].getValue();
		core.wet = params[MIX_PARAM].getValue();
		core.dampGate = inputs[DAMP_INPUT].getVoltage() > 1.f;

		// +/-5 V -> -1..1 through the shared core and back.
		const float in = inputs[AUDIO_INPUT].getVoltage() / 5.f;
		const float out = core.process(in, args.sampleTime);
		outputs[AUDIO_OUTPUT].setVoltage(clamp(out, -1.f, 1.f) * 5.f);

		// LED follows the string's energy, as on hardware.
		lights[ENERGY_LIGHT].setBrightness(core.ledLevel());
	}
};

struct KarplusWidget : ModuleWidget {
	KarplusWidget(Karplus* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/mod2-karplus.svg")));
		// 4 HP Mod1/Mod2 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.03f, 21.7f)), module, Karplus::PITCH_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 40.06f)), module, Karplus::DAMP_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 58.42f)), module, Karplus::MIX_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, Karplus::MODE_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, Karplus::ENERGY_LIGHT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, Karplus::PLUCK_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.3f)), module, Karplus::DAMP_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, Karplus::AUDIO_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, Karplus::AUDIO_INPUT));
	}

	// Excitation colour is the firmware's second shift layer (BUTTON + POT2);
	// with no spare knob on the 4 HP panel it lives here in the context menu.
	void appendContextMenu(Menu* menu) override {
		Karplus* module = getModule<Karplus>();
		menu->addChild(new MenuSeparator);
		ui::Slider* slider = new ui::Slider;
		slider->quantity = module->paramQuantities[Karplus::COLOUR_PARAM];
		slider->box.size.x = 200.f;
		menu->addChild(slider);
	}
};

Model* modelKarplus = createModel<Karplus, KarplusWidget>("mod2-karplus");
