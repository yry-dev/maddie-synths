#include "plugin.hpp"
#include <RandomLagCore.h>  // Shared walk+lag core (also used by mod1-random-lag firmware)

/*
	RandomLag — random walk CV generator with lagged follower output.

	Port of firmwares/mod1-random-lag/mod1-random-lag.ino (Rob Heel, for Hagiwo Mod1).

	The module runs a random walk (or gravity walk) and outputs it on F4. F2 is an
	exponentially-smoothed follower that trails F4, creating slowly drifting
	relationships between the two outputs useful for ambient cross-fading.

	Panel layout (mirrors the Mod1 hardware: 3 pots, 1 button, 1 LED, 4 jacks):
		POT1 RATE  — exponential step rate (slow..fast)
		POT2 BIAS  — shifts both outputs up or down
		POT3 CHAOS — step size / depth
		BUTTON     — latch: gravity mode pulls the walk back toward 0
		F1 in      — lag-amount CV: how tightly F2 follows F4 (0V = independent)
		F3 in      — chaos depth CV (additive to CHAOS knob)
		F2 out     — lagged walk (0..10V)
		F4 out     — main walk (0..10V)
		LED        — brightness tracks the main walk value
*/

struct RandomLag : Module {
	enum ParamId {
		RATE_PARAM,
		BIAS_PARAM,
		CHAOS_PARAM,
		GRAV_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		F1_INPUT,   // lag-amount CV (0V = F2 independent of F4)
		F3_INPUT,   // chaos depth CV (additive)
		INPUTS_LEN
	};
	enum OutputId {
		F2_OUTPUT,  // lagged walk (0..10V)
		F4_OUTPUT,  // main random walk (0..10V)
		OUTPUTS_LEN
	};
	enum LightId {
		WALK_LIGHT,
		LIGHTS_LEN
	};

	sc::RandomLagVoice voice;

	RandomLag() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

		configParam(RATE_PARAM,  0.f, 1.f, 0.3f, "Rate (step speed)");
		configParam(BIAS_PARAM,  0.f, 1.f, 0.5f, "Bias (output offset)");
		configParam(CHAOS_PARAM, 0.f, 1.f, 0.5f, "Chaos (step size)");
		configSwitch(GRAV_PARAM, 0.f, 1.f, 0.f,  "Gravity mode", {"Off", "On"});

		configInput(F1_INPUT, "F1 lag-amount CV");
		configInput(F3_INPUT, "F3 chaos depth CV");
		configOutput(F2_OUTPUT, "F2 lagged walk");
		configOutput(F4_OUTPUT, "F4 main walk");
		configLight(WALK_LIGHT, "Walk value");
	}

	void onReset() override {
		voice.reset();
	}

	void process(const ProcessArgs& args) override {
		// Gravity mode from latch button.
		voice.gravityMode = params[GRAV_PARAM].getValue() > 0.5f;

		// F1 lag-amount CV (firmware: A3 analog in). 0V = F2 independent of F4,
		// higher = tighter following. Maps 0..10V -> core lagCV 0..1.
		const float lagCV = clamp(inputs[F1_INPUT].getVoltage() / 10.f, 0.f, 1.f);

		// F3 chaos CV: 0..5V normalised to 0..1, additive to CHAOS knob.
		const float cv3 = clamp(inputs[F3_INPUT].getVoltage() / 5.f, 0.f, 1.f);

		const sc::RandomLagParams rp = sc::randomLagMapParams(
			params[RATE_PARAM].getValue(),
			params[BIAS_PARAM].getValue(),
			params[CHAOS_PARAM].getValue(),
			lagCV,
			cv3);

		// Advance one audio sample (dt = args.sampleTime). The firmware has no
		// reset trigger, so the walk runs freely.
		voice.process(args.sampleTime, /*trigRose=*/false, rp);

		// Scale normalised [0..1] outputs to 0..10V.
		const float walkV   = voice.walkOut(rp.bias)   * 10.f;
		const float laggedV = voice.laggedOut(rp.bias) * 10.f;

		outputs[F4_OUTPUT].setVoltage(walkV);
		outputs[F2_OUTPUT].setVoltage(laggedV);

		// LED tracks main walk brightness.
		lights[WALK_LIGHT].setBrightness(voice.walkOut(rp.bias));
	}
};

struct RandomLagWidget : ModuleWidget {
	RandomLagWidget(RandomLag* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/mod1-random-lag.svg")));

		// 4 HP panel (19.8 mm): hole centres from the mod1-random-lag KiCad faceplate
		// (panel-local mm, scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		// Three pots.
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.03f, 21.70f)), module, RandomLag::RATE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 40.06f)), module, RandomLag::BIAS_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 58.42f)), module, RandomLag::CHAOS_PARAM));

		// Latching button (gravity mode) + walk-value LED.
		addParam(createParamCentered<VCVLatch>(mm2px(Vec(5.19f, 78.57f)), module, RandomLag::GRAV_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, RandomLag::WALK_LIGHT));

		// Jacks: F1 in (top-left), F2 out (top-right), F3 in (bottom-left), F4 out (bottom-right).
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, RandomLag::F1_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.30f)), module, RandomLag::F2_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, RandomLag::F3_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, RandomLag::F4_OUTPUT));
	}
};

Model* modelRandomLag = createModel<RandomLag, RandomLagWidget>("mod1-random-lag");
