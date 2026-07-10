#include "plugin.hpp"
#include <SamplePlayerCore.h>  // Shared PCM playback voice
// Baked PCM sample bank (same data the firmware plays). sample.h uses the
// Arduino PROGMEM macro + <pgmspace.h>; the desktop build resolves those via
// src/compat (see the Makefile). const arrays have internal linkage.
#include "../../firmwares/mod2-sample/sample.h"  // samples[18], sampleLens[18]

/*
	Sample — one-shot 16-bit PCM sample player.

	Port of firmwares/mod2-sample/mod2-sample.ino (HAGIWO Mod2, RP2350).

	Plays one of up to 18 baked samples on each trigger. Two-stage selection:
	GROUP (POT2) picks one of three banks of six, INDEX (POT3 + CV) picks within
	the bank; a gate on IN2 adds 6 to the index (wrapping), like the hardware.
	Variable speed (0.5-1.5x) with the shared sc::SamplePlayer's interpolation.

	Source PCM is 44.1 kHz mono; the read pointer advances at the source rate
	relative to Rack's engine rate, so pitch is sample-rate independent.

	Mod2 hardware mapping:
		POT1 SPEED (0.5-1.5x)   POT2 GROUP (1-6/7-12/13-18)   POT3 INDEX (6)
		BTN  manual trigger     LED  20 ms trigger pulse
		IN1  trigger (rising)   IN2  +6 sample select (gate)
		OUT  audio              CV   index (summed with POT3)
*/

static const float SP_SRC_RATE = 44100.0f;  // sample.h PCM rate
static const int SP_NUM_SAMPLES = 18;

struct SamplePlayerModule : Module {
	enum ParamId { SPEED_PARAM, GROUP_PARAM, INDEX_PARAM, TRIG_PARAM, PARAMS_LEN };
	enum InputId { TRIG_INPUT, PLUS6_INPUT, INDEX_CV_INPUT, INPUTS_LEN };
	enum OutputId { AUDIO_OUTPUT, OUTPUTS_LEN };
	enum LightId { TRIG_LIGHT, LIGHTS_LEN };

	sc::SamplePlayer player;
	dsp::SchmittTrigger gateTrigger;
	dsp::BooleanTrigger buttonTrigger;
	dsp::PulseGenerator ledPulse;

	SamplePlayerModule() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(SPEED_PARAM, 0.5f, 1.5f, 1.0f, "Playback speed", "x");
		configParam(GROUP_PARAM, 0.f, 1.f, 0.f, "Sample group (bank of 6)");
		configParam(INDEX_PARAM, 0.f, 1.f, 0.f, "Sample index in group");
		configButton(TRIG_PARAM, "Trigger");
		configInput(TRIG_INPUT, "IN1 trigger");
		configInput(PLUS6_INPUT, "+6 sample select (gate)");
		configInput(INDEX_CV_INPUT, "Sample index CV");
		configOutput(AUDIO_OUTPUT, "Audio");
	}

	void onReset() override { player.stop(); }

	int selectIndex() {
		float g = params[GROUP_PARAM].getValue();
		int group = (g < 1.f / 3.f) ? 0 : (g < 2.f / 3.f) ? 1 : 2;

		float sub = params[INDEX_PARAM].getValue() + inputs[INDEX_CV_INPUT].getVoltage() / 10.f;
		int subIndex = (int)std::round(clamp(sub, 0.f, 1.f) * 5.f);

		int index = group * 6 + subIndex;
		if (inputs[PLUS6_INPUT].getVoltage() > 1.f) {
			index += 6;
			if (index >= SP_NUM_SAMPLES) index %= 6;
		}
		return clamp(index, 0, SP_NUM_SAMPLES - 1);
	}

	void startPlayback(const ProcessArgs& args) {
		int idx = selectIndex();
		const uint8_t* base = samples[idx];
		uint32_t len = sampleLens[idx];
		if (!base || len == 0) return;
		double step = (double)params[SPEED_PARAM].getValue() * SP_SRC_RATE / args.sampleRate;
		player.trigger(base, len, step, false);
		ledPulse.trigger(0.02f);  // 20 ms trigger LED
	}

	void process(const ProcessArgs& args) override {
		bool fire = buttonTrigger.process(params[TRIG_PARAM].getValue() > 0.5f);
		fire |= gateTrigger.process(inputs[TRIG_INPUT].getVoltage(), 0.1f, 1.f);
		if (fire) startPlayback(args);

		float out = player.process();
		outputs[AUDIO_OUTPUT].setVoltage(out * 5.f);  // -1..1 -> +/-5V
		lights[TRIG_LIGHT].setBrightnessSmooth(ledPulse.process(args.sampleTime) ? 1.f : 0.f, args.sampleTime);
	}
};

struct SamplePlayerWidget : ModuleWidget {
	SamplePlayerWidget(SamplePlayerModule* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/mod2-sample.svg")));

		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.03f, 21.70f)), module, SamplePlayerModule::SPEED_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 40.06f)), module, SamplePlayerModule::GROUP_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 58.42f)), module, SamplePlayerModule::INDEX_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, SamplePlayerModule::TRIG_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, SamplePlayerModule::TRIG_LIGHT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, SamplePlayerModule::TRIG_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.30f)), module, SamplePlayerModule::PLUS6_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, SamplePlayerModule::AUDIO_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, SamplePlayerModule::INDEX_CV_INPUT));
	}
};

Model* modelSample = createModel<SamplePlayerModule, SamplePlayerWidget>("mod2-sample");
