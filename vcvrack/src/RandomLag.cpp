#include "plugin.hpp"
#include <RandomLagCore.h>  // Shared walk+lag core (also used by mod1-random-lag firmware)

/*
	RandomLag — random walk CV generator with lagged follower output.

	Port of firmwares/mod1-random-lag/mod1-random-lag.ino (Rob Heel, for Hagiwo Mod1).

	The module runs a random walk (or gravity walk) and outputs it on F4. F2 is an
	exponentially-smoothed follower that trails F4, creating slowly drifting
	relationships between the two outputs useful for ambient cross-fading.

	Panel layout (mirrors firmware):
		RATE   — exponential step rate (slow..fast)
		BIAS   — shifts both outputs up or down
		CHAOS  — step size / depth
		LAG    — how loosely F2 follows F4 (0 = tight, 1 = wide/independent)
		GRAV   — latch: gravity mode pulls walk back toward 0
		F1 in  — trigger: rising edge resets walk to mid-scale
		F3 in  — chaos depth CV (0..5V additive to CHAOS knob)
		F2 out — lagged walk (0..10V)
		F4 out — main walk (0..10V)
		LED    — brightness tracks main walk value
*/

struct RandomLag : Module {
	enum ParamId {
		RATE_PARAM,
		BIAS_PARAM,
		CHAOS_PARAM,
		LAG_PARAM,
		GRAV_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		F1_INPUT,   // trigger — resets walk to mid-scale on rising edge
		F3_INPUT,   // chaos depth CV (0..5V)
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
	dsp::SchmittTrigger resetTrigger;

	RandomLag() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

		configParam(RATE_PARAM,  0.f, 1.f, 0.3f, "Rate (step speed)");
		configParam(BIAS_PARAM,  0.f, 1.f, 0.5f, "Bias (output offset)");
		configParam(CHAOS_PARAM, 0.f, 1.f, 0.5f, "Chaos (step size)");
		// LAG knob: 0 = tight following, 1 = wide/independent (inverted vs cv1 mapping)
		configParam(LAG_PARAM,   0.f, 1.f, 0.f,  "Lag (F2 independence from F4)");
		configSwitch(GRAV_PARAM, 0.f, 1.f, 0.f,  "Gravity mode", {"Off", "On"});

		configInput(F1_INPUT, "F1 reset trigger");
		configInput(F3_INPUT, "F3 chaos depth CV");
		configOutput(F2_OUTPUT, "F2 lagged walk");
		configOutput(F4_OUTPUT, "F4 main walk");
		configLight(WALK_LIGHT, "Walk value");
	}

	void onReset() override {
		voice.reset();
	}

	void process(const ProcessArgs& args) override {
		// F1: reset walk to mid-scale on rising edge (>1V threshold).
		const bool trigRose = resetTrigger.process(
			inputs[F1_INPUT].getVoltage(), 0.1f, 1.f);

		// Gravity mode from latch button.
		voice.gravityMode = params[GRAV_PARAM].getValue() > 0.5f;

		// LAG knob is inverted: 0 = tight (cv1=1), 1 = independent (cv1=0).
		// This maps the knob label intuitively — more lag = more independence.
		const float lagCV = 1.f - params[LAG_PARAM].getValue();

		// F3 chaos CV: 0..5V normalised to 0..1, additive to CHAOS knob.
		const float cv3 = clamp(inputs[F3_INPUT].getVoltage() / 5.f, 0.f, 1.f);

		const sc::RandomLagParams rp = sc::randomLagMapParams(
			params[RATE_PARAM].getValue(),
			params[BIAS_PARAM].getValue(),
			params[CHAOS_PARAM].getValue(),
			lagCV,
			cv3);

		// Advance one audio sample (dt = args.sampleTime).
		voice.process(args.sampleTime, trigRose, rp);

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
		setPanel(createPanel(asset::plugin(pluginInstance, "res/RandomLag.svg")));

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		const float cx = 15.24f; // 6 HP center

		// LED — walk value indicator.
		addChild(createLightCentered<MediumLight<GreenLight>>(
			mm2px(Vec(cx, 18.5f)), module, RandomLag::WALK_LIGHT));

		// Four knobs (tighter spacing than Butterfly to fit the extra LAG knob).
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(cx, 30.f)),  module, RandomLag::RATE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(cx, 44.f)),  module, RandomLag::BIAS_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(cx, 58.f)),  module, RandomLag::CHAOS_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(cx, 72.f)),  module, RandomLag::LAG_PARAM));

		// Push button (latching: toggles gravity mode).
		addParam(createParamCentered<VCVLatch>(mm2px(Vec(cx, 84.f)), module, RandomLag::GRAV_PARAM));

		// Four jacks: F1 in (trigger), F3 in (chaos CV), F2 out (lagged), F4 out (walk).
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.16f, 98.f)),  module, RandomLag::F1_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(20.32f, 98.f)),  module, RandomLag::F2_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.16f, 113.f)), module, RandomLag::F3_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(20.32f, 113.f)), module, RandomLag::F4_OUTPUT));
	}
};

Model* modelRandomLag = createModel<RandomLag, RandomLagWidget>("RandomLag");
