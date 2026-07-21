// WIP: Claude-generated maddie synths original (no upstream Hagiwo/Rob
// Scape firmware). Excluded from the VCV Rack build -- see WIP_SOURCES in
// the Makefile and the WIP block in plugin.cpp for how to re-enable it.
#include "plugin.hpp"
#include <SpectralFreezeCore.h>  // Shared spectral-freeze DSP (also used by mod2-spectral-freeze firmware)

/*
	Spectral Freeze — FFT-domain infinite sustain / phase-vocoder freeze.

	Port of firmwares/mod2-spectral-freeze/mod2-spectral-freeze.ino (HAGIWO
	Mod2, RP2350).

	Mirrors the Mod2 hardware: 3 pots, 1 push button, 1 PWM LED, and the Mod2
	jack set (IN1, IN2, CV, OUT). For the Spectral Freeze firmware:
		POT1 (A0) -> Shimmer (per-bin phase-randomisation depth)
		POT2 (A1) -> Spectral tilt (dark <-> bright)
		POT3 (A2) -> unavailable on hardware (pin doubles as the audio input)
		BUTTON    -> mode: Single frame / 4-frame averaged / Drifting
		LED       -> solid when frozen; breathes at the shimmer rate
		IN1       -> capture a fresh spectrum (crossfaded in the freq domain)
		IN2       -> freeze gate (>1 V = frozen)
		CV        -> AUDIO INPUT (sampled at the audio rate on hardware)
		OUT       -> audio output

	Deviations from hardware: the firmware's wet/dry (frozen vs live) lives on a
	shift layer (hold BUTTON + turn POT1) because POT3's pin is the audio input;
	here the physically-present-but-dead third knob becomes a proper Mix control.
	On hardware the FFT frames run on the second core behind ring buffers; in
	Rack the phase-vocoder burst runs inline at each 256-sample hop boundary. The
	freeze mode persists in the patch (firmware: flash). Reconstruction latency
	is ~one FFT window (768 samples), irrelevant for a freeze effect.
*/

struct SpectralFreeze : Module {
	enum ParamId {
		SHIMMER_PARAM,
		TILT_PARAM,
		MIX_PARAM,
		MODE_PARAM,   // momentary button — cycles Single / Averaged / Drifting
		ATTACK_PARAM, // freeze attack/blend time — context-menu slider (firmware BUTTON+POT2)
		PARAMS_LEN
	};
	enum InputId {
		CAPTURE_INPUT, // IN1 — capture a fresh spectrum (trigger)
		FREEZE_INPUT,  // IN2 — freeze gate (>1 V = frozen)
		AUDIO_INPUT,   // CV jack — audio in
		INPUTS_LEN
	};
	enum OutputId {
		AUDIO_OUTPUT,  // OUT
		OUTPUTS_LEN
	};
	enum LightId {
		FREEZE_LIGHT,
		LIGHTS_LEN
	};

	// Freeze DSP lives in the shared core (same STFT as the firmware).
	sc::SpectralFreezeCore core;

	dsp::SchmittTrigger captureTrigger;
	dsp::BooleanTrigger modeButton;

	SpectralFreeze() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(SHIMMER_PARAM, 0.f, 1.f, 0.3f, "Shimmer (phase animation)", "%", 0.f, 100.f);
		configParam(TILT_PARAM, 0.f, 1.f, 0.5f, "Spectral tilt (dark → bright)");
		configParam(MIX_PARAM, 0.f, 1.f, 1.f, "Wet/dry mix (frozen vs live)", "%", 0.f, 100.f);
		configButton(MODE_PARAM, "Mode (Single / Averaged / Drifting)");
		configParam(ATTACK_PARAM, 0.f, 1.f, 0.3f, "Freeze attack/blend time (5 ms → 2 s)");

		configInput(CAPTURE_INPUT, "IN1 capture trigger (grab a new spectrum)");
		configInput(FREEZE_INPUT, "IN2 freeze gate (>1 V = frozen)");
		configInput(AUDIO_INPUT, "Audio");
		configOutput(AUDIO_OUTPUT, "Audio");

		configBypass(AUDIO_INPUT, AUDIO_OUTPUT);
		core.init();
	}

	void onReset() override {
		core.reset();
		core.mode = sc::SPFREEZE_SINGLE;
	}

	// The freeze mode persists with the patch (the firmware stores it in flash).
	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "freezeMode", json_integer(core.mode));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* modeJ = json_object_get(rootJ, "freezeMode");
		if (modeJ)
			core.mode = (uint8_t)clamp((int)json_integer_value(modeJ), 0,
			                           sc::SPFREEZE_MODE_COUNT - 1);
	}

	void process(const ProcessArgs& args) override {
		if (modeButton.process(params[MODE_PARAM].getValue() > 0.5f))
			core.mode = (core.mode + 1) % sc::SPFREEZE_MODE_COUNT;

		// IN1 rising edge captures a fresh spectrum (crossfaded in the core).
		if (captureTrigger.process(inputs[CAPTURE_INPUT].getVoltage(), 0.1f, 1.f))
			core.triggerCapture();

		// Same control mappings as the firmware.
		core.shimmer = sc::spectralShimmerRad(params[SHIMMER_PARAM].getValue());
		core.tiltSlope = sc::spectralTiltSlope(params[TILT_PARAM].getValue());
		core.attackSec = sc::spectralAttackSec(params[ATTACK_PARAM].getValue());
		core.wet = params[MIX_PARAM].getValue();
		// IN2 gate freezes; if unpatched the module stays live (dry through).
		core.freeze = inputs[FREEZE_INPUT].getVoltage() > 1.f;

		// +/-5 V -> -1..1 through the shared core and back.
		const float in = inputs[AUDIO_INPUT].getVoltage() / 5.f;
		const float out = core.process(in, args.sampleTime);
		outputs[AUDIO_OUTPUT].setVoltage(clamp(out, -1.5f, 1.5f) * 5.f);

		// LED solid/breathing when frozen, as on hardware.
		lights[FREEZE_LIGHT].setBrightness(core.ledLevel());
	}
};

struct SpectralFreezeWidget : ModuleWidget {
	SpectralFreezeWidget(SpectralFreeze* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/mod2-spectral-freeze.svg")));
		// 4 HP Mod1/Mod2 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.03f, 21.7f)), module, SpectralFreeze::SHIMMER_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 40.06f)), module, SpectralFreeze::TILT_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 58.42f)), module, SpectralFreeze::MIX_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, SpectralFreeze::MODE_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, SpectralFreeze::FREEZE_LIGHT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, SpectralFreeze::CAPTURE_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.3f)), module, SpectralFreeze::FREEZE_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, SpectralFreeze::AUDIO_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, SpectralFreeze::AUDIO_INPUT));
	}

	// Attack/blend is the firmware's second shift layer (BUTTON + POT2); the
	// 4 HP panel has no spare knob, so it lives here (same as mod2-granular).
	void appendContextMenu(Menu* menu) override {
		SpectralFreeze* module = getModule<SpectralFreeze>();
		menu->addChild(new MenuSeparator);
		ui::Slider* slider = new ui::Slider;
		slider->quantity = module->paramQuantities[SpectralFreeze::ATTACK_PARAM];
		slider->box.size.x = 200.f;
		menu->addChild(slider);
	}
};

Model* modelSpectralFreeze = createModel<SpectralFreeze, SpectralFreezeWidget>("mod2-spectral-freeze");
