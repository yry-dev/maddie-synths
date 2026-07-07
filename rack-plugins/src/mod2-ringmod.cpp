#include "plugin.hpp"
#include <RingModCore.h>  // Shared ring mod DSP (also used by mod2-ringmod firmware)

/*
	Ring Mod — carrier-oscillator ring modulator.

	Port of firmwares/mod2-ringmod/mod2-ringmod.ino (HAGIWO Mod2, RP2350).

	Mirrors the Mod2 hardware: 3 pots, 1 push button, 1 PWM LED, and the Mod2
	jack set (IN1, IN2, CV, OUT). For the Ring Mod firmware:
		POT1 (A0) -> Carrier freq (0.5 Hz - 5 kHz) / Track ratio (0.25 - 4)
		POT2 (A1) -> Carrier shape morph (sine -> triangle -> square)
		POT3 (A2) -> unavailable on hardware (pin doubles as the audio input)
		BUTTON    -> mode: Fixed / Track (carrier follows input pitch) / S&H
		LED       -> blinks at the carrier rate
		IN1       -> carrier hard-sync (S&H: samples a new random carrier)
		IN2       -> octave-drop gate (>1 V = carrier an octave down)
		CV        -> AUDIO INPUT (sampled at the audio rate on hardware)
		OUT       -> audio output

	Deviations from hardware: the firmware's wet/dry lives on a shift layer
	(hold BUTTON + turn POT1) because POT3's pin is the audio input; here the
	physically-present-but-dead third knob becomes a proper Mix control. The
	second shift layer (hold BUTTON + turn POT2 = AM <-> ring-mod blend) has
	no spare knob at all, so the AM blend lives in the context menu. The mode
	persists in the patch (firmware: flash).
*/

struct RingMod : Module {
	enum ParamId {
		FREQ_PARAM,
		SHAPE_PARAM,
		MIX_PARAM,
		MODE_PARAM,   // momentary button — cycles Fixed / Track / S&H
		BLEND_PARAM,  // context-menu slider (hardware: BUTTON + POT2)
		PARAMS_LEN
	};
	enum InputId {
		SYNC_INPUT,   // IN1 — carrier hard-sync / S&H trigger
		OCTAVE_INPUT, // IN2 — octave-drop gate (>1 V = -1 oct)
		AUDIO_INPUT,  // CV jack — audio in
		INPUTS_LEN
	};
	enum OutputId {
		AUDIO_OUTPUT, // OUT
		OUTPUTS_LEN
	};
	enum LightId {
		CARRIER_LIGHT,
		LIGHTS_LEN
	};

	// Ring mod state lives in the shared core (same DSP as the firmware).
	sc::RingModCore core;

	dsp::SchmittTrigger syncTrigger;
	dsp::BooleanTrigger modeButton;

	RingMod() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(FREQ_PARAM, 0.f, 1.f, 0.55f, "Carrier freq (0.5 Hz → 5 kHz; Track: ratio 0.25 → 4)");
		configParam(SHAPE_PARAM, 0.f, 1.f, 0.f, "Carrier shape (sine → triangle → square)", "%", 0.f, 100.f);
		configParam(MIX_PARAM, 0.f, 1.f, 1.f, "Wet/dry mix", "%", 0.f, 100.f);
		configButton(MODE_PARAM, "Mode (Fixed / Track / S&H)");
		configParam(BLEND_PARAM, 0.f, 1.f, 0.f, "AM ↔ ring-mod blend (dry leak)", "%", 0.f, 100.f);

		configInput(SYNC_INPUT, "IN1 carrier hard-sync / S&H trigger");
		configInput(OCTAVE_INPUT, "IN2 octave-drop gate (>1 V = −1 oct)");
		configInput(AUDIO_INPUT, "Audio");
		configOutput(AUDIO_OUTPUT, "Audio");

		configBypass(AUDIO_INPUT, AUDIO_OUTPUT);
	}

	void onReset() override {
		core.reset();
		core.mode = sc::RINGMOD_FIXED;
	}

	// The mode persists with the patch (the firmware stores it in flash).
	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "ringModMode", json_integer(core.mode));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* modeJ = json_object_get(rootJ, "ringModMode");
		if (modeJ)
			core.mode = (uint8_t)clamp((int)json_integer_value(modeJ), 0,
			                           sc::RINGMOD_MODE_COUNT - 1);
	}

	void process(const ProcessArgs& args) override {
		if (modeButton.process(params[MODE_PARAM].getValue() > 0.5f))
			core.mode = (core.mode + 1) % sc::RINGMOD_MODE_COUNT;

		// IN1: hard-sync the carrier (S&H mode: new random frequency).
		if (syncTrigger.process(inputs[SYNC_INPUT].getVoltage(), 0.1f, 1.f))
			core.trigger();

		// Track-mode pitch search: runs every ~50 ms once the decimated
		// buffer fills (the firmware calls this from loop()); no-op otherwise.
		core.analyzePitch(args.sampleRate);

		// Same pot mappings as the firmware.
		const float freqPot = params[FREQ_PARAM].getValue();
		core.carrierHz = sc::ringModCarrierHz(freqPot);
		core.trackRatio = sc::ringModTrackRatio(freqPot);
		core.shape = params[SHAPE_PARAM].getValue();
		core.amBlend = params[BLEND_PARAM].getValue();
		core.wet = params[MIX_PARAM].getValue();
		core.octaveDrop = inputs[OCTAVE_INPUT].getVoltage() > 1.f;

		// +/-5 V -> -1..1 through the shared core and back.
		const float in = inputs[AUDIO_INPUT].getVoltage() / 5.f;
		const float out = core.process(in, args.sampleTime);
		outputs[AUDIO_OUTPUT].setVoltage(clamp(out, -1.f, 1.f) * 5.f);

		// LED blinks at the carrier rate, as on hardware.
		lights[CARRIER_LIGHT].setBrightness(core.ledLevel());
	}
};

struct RingModWidget : ModuleWidget {
	RingModWidget(RingMod* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/mod2-ringmod.svg")));
		// 4 HP Mod1/Mod2 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.03f, 21.7f)), module, RingMod::FREQ_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 40.06f)), module, RingMod::SHAPE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 58.42f)), module, RingMod::MIX_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, RingMod::MODE_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, RingMod::CARRIER_LIGHT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, RingMod::SYNC_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.3f)), module, RingMod::OCTAVE_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, RingMod::AUDIO_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, RingMod::AUDIO_INPUT));
	}

	// The AM blend is the firmware's second shift layer (BUTTON + POT2);
	// with no spare knob on the 4 HP panel it lives here in the context menu.
	void appendContextMenu(Menu* menu) override {
		RingMod* module = getModule<RingMod>();
		menu->addChild(new MenuSeparator);
		ui::Slider* slider = new ui::Slider;
		slider->quantity = module->paramQuantities[RingMod::BLEND_PARAM];
		slider->box.size.x = 200.f;
		menu->addChild(slider);
	}
};

Model* modelRingMod = createModel<RingMod, RingModWidget>("mod2-ringmod");
