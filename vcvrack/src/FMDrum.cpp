#include "plugin.hpp"
#include <FmDrumCore.h>  // Shared FM-drum voice (also used by the mod2-fm_drum firmware)

/*
	FMDrum — two-operator FM percussion voice.

	Port of firmwares/mod2-fm_drum/mod2-fm_drum.ino (HAGIWO Mod2, RP2350).

	The firmware multiplexes three pots across two button-selected modes
	(pickup + EEPROM) to reach six controls, of which five are distinct (POT3
	"mod index" is shared by both modes):

		Mode 0  POT1 -> Pitch        POT2 -> Op ratio     POT3 -> Mod index
		Mode 1  POT1 -> Decay        POT2 -> Ratio env    POT3 -> Mod index

	That mode/pickup/EEPROM machinery only exists because the hardware has three
	physical pots. In Rack there is no such constraint, so this port collapses the
	dual-mode scheme into FIVE individual knobs laid out in a grid (Rack persists
	their values for free — no mode button, no pickup, no flash writes). The FM
	synthesis itself is the shared sc::FmDrumVoice core, identical to the firmware.

	The Mod2 jack set is mirrored: IN1 = trigger, IN2 = accent (HIGH cuts -6 dB),
	CV = mod-index modulation (the firmware shares this pin with POT3), OUT = audio.

	Deviations from the firmware (for software ergonomics; documented):
	  - Knob directions are non-inverted (CW = more). The firmware inverts mod
	    index / decay / ratio-env on the pots purely as a hardware-pickup choice.
	  - Like the Claves port, the strike is rendered live at Rack's sample rate
	    (sample-rate independent) instead of baked into a 4096-sample table.
	  - Parameters are frozen at strike time, matching the firmware's bake-on-
	    trigger behavior; knob moves take effect on the next trigger.
*/

struct FMDrum : Module {
	enum ParamId {
		PITCH_PARAM,
		RATIO_PARAM,
		INDEX_PARAM,
		DECAY_PARAM,
		RATIOENV_PARAM,
		TRIG_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		TRIG_INPUT,     // IN1
		ACCENT_INPUT,   // IN2 (HIGH = -6 dB)
		INDEX_CV_INPUT, // CV (shared with POT3 on hardware)
		INPUTS_LEN
	};
	enum OutputId {
		AUDIO_OUTPUT,   // OUT
		OUTPUTS_LEN
	};
	enum LightId {
		ENV_LIGHT,
		LIGHTS_LEN
	};

	// Voice state lives in the shared core (same synthesis as the firmware).
	sc::FmDrumVoice voice;

	dsp::SchmittTrigger gateTrigger;
	dsp::BooleanTrigger buttonTrigger;

	FMDrum() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		// Knobs are 0..1; engine units (matching the firmware ranges) are derived
		// in strike(). Defaults pick a usable FM percussion tone.
		configParam(PITCH_PARAM, 0.f, 1.f, 0.15f, "Pitch");
		configParam(RATIO_PARAM, 0.f, 1.f, 0.2f, "Operator ratio");
		configParam(INDEX_PARAM, 0.f, 1.f, 0.3f, "Modulation index");
		configParam(DECAY_PARAM, 0.f, 1.f, 0.5f, "Decay");
		configParam(RATIOENV_PARAM, 0.f, 1.f, 0.f, "Ratio envelope");
		configButton(TRIG_PARAM, "Manual trigger");

		configInput(TRIG_INPUT, "IN1 trigger");
		configInput(ACCENT_INPUT, "Accent (-6 dB when high)");
		configInput(INDEX_CV_INPUT, "Mod index CV");
		configOutput(AUDIO_OUTPUT, "Audio");
	}

	// Sample knobs/CV/accent and start a new strike (firmware onTrigger() bake).
	void strike() {
		const float f0 = 30.f + 1170.f * params[PITCH_PARAM].getValue();        // 30..1200 Hz
		const float opRatio = 0.5f + 7.5f * params[RATIO_PARAM].getValue();     // 0.5..8
		const float modIndex = clamp(1.f + 9.f * params[INDEX_PARAM].getValue() // 1..10 (+CV)
		                             + inputs[INDEX_CV_INPUT].getVoltage(), 1.f, 10.f);
		const float decayRate = 0.5f + 9.5f * params[DECAY_PARAM].getValue();   // 0.5..10
		const float ratioEnv = params[RATIOENV_PARAM].getValue();              // 0..1
		const float accentLevel = inputs[ACCENT_INPUT].getVoltage() > 1.f ? 0.5f : 1.f;

		voice.setParams(f0, opRatio, modIndex, decayRate, ratioEnv,
		                sc::kFmDrumIndexEnv, accentLevel);
		voice.strike();
	}

	void onReset() override {
		voice.reset();
	}

	void process(const ProcessArgs& args) override {
		// Fire on IN1 rising edge or a button press.
		const bool gate = gateTrigger.process(inputs[TRIG_INPUT].getVoltage(), 0.1f, 1.f);
		const bool button = buttonTrigger.process(params[TRIG_PARAM].getValue() > 0.5f);
		if (gate || button)
			strike();

		// One sample from the shared core: audio in -1..1, envelope in 0..1.
		const sc::FmDrumFrame f = voice.process(args.sampleTime);
		outputs[AUDIO_OUTPUT].setVoltage(f.audio * 5.f); // -1..1 -> +/-5V (Vpp 10V)
		lights[ENV_LIGHT].setBrightnessSmooth(f.env, args.sampleTime);
	}
};

struct FMDrumWidget : ModuleWidget {
	FMDrumWidget(FMDrum* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/FMDrum.svg")));

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		// 8 HP panel: two knob columns + an envelope LED.
		const float cxL = 13.2f;   // left knob column
		const float cxR = 27.4f;   // right knob column
		const float cx = 20.32f;   // 8 HP center

		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(cx, 22.f)), module, FMDrum::ENV_LIGHT));

		// Knob grid (5 distinct params, collapsed from the firmware's dual modes).
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(cxL, 40.f)), module, FMDrum::PITCH_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(cxR, 40.f)), module, FMDrum::RATIO_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(cxL, 60.f)), module, FMDrum::INDEX_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(cxR, 60.f)), module, FMDrum::DECAY_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(cxL, 80.f)), module, FMDrum::RATIOENV_PARAM));

		addParam(createParamCentered<VCVButton>(mm2px(Vec(cxR, 80.f)), module, FMDrum::TRIG_PARAM));

		// Jacks: row 1 = trigger / accent / index-CV inputs, row 2 = OUT.
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.0f, 100.f)), module, FMDrum::TRIG_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(20.32f, 100.f)), module, FMDrum::ACCENT_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(30.64f, 100.f)), module, FMDrum::INDEX_CV_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(30.64f, 114.f)), module, FMDrum::AUDIO_OUTPUT));
	}
};

Model* modelFMDrum = createModel<FMDrum, FMDrumWidget>("FMDrum");
