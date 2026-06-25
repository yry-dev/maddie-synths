#include "plugin.hpp"
#include <DualADEnvCore.h>  // Shared AD-envelope core (also used by mod1-dual-ad-env firmware)

/*
	DualADEnv — dual attack-decay envelope generator.

	Port of firmwares/mod1-dual-ad-env/mod1-dual-ad-env.ino (Rob Scape / Maddie Synths,
	for Hagiwo Mod1).

	Two independent AD envelopes share the Attack, Release, and Vary knobs. Each
	envelope has its own trigger input; Envelope 1 can also be triggered by the
	front-panel button (mirrors the hardware push button). Random timing variation
	is applied at trigger time: the Vary knob scales the per-trigger deviation,
	with shorter attack/release settings receiving proportionally more variation.

	Panel jack mapping (mirrors firmware):
	  F1 (left-top)   IN  — Trigger 1
	  F2 (right-top)  OUT — Envelope 1 (0..10 V)
	  F3 (left-bot)   IN  — Trigger 2
	  F4 (right-bot)  OUT — Envelope 2 (0..10 V)

	LED tracks Envelope 1 level.
*/

struct DualADEnv : Module {
	enum ParamId {
		ATTACK_PARAM,
		RELEASE_PARAM,
		VARY_PARAM,
		TRIG1_BUTTON,
		PARAMS_LEN
	};
	enum InputId {
		F1_INPUT,   // Trigger 1
		F3_INPUT,   // Trigger 2
		INPUTS_LEN
	};
	enum OutputId {
		F2_OUTPUT,  // Envelope 1
		F4_OUTPUT,  // Envelope 2
		OUTPUTS_LEN
	};
	enum LightId {
		ENV1_LIGHT,
		LIGHTS_LEN
	};

	sc::ADEnvVoice env1;
	sc::ADEnvVoice env2;

	dsp::SchmittTrigger trig1In;
	dsp::SchmittTrigger trig2In;
	dsp::BooleanTrigger  trig1Button;

	DualADEnv() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(ATTACK_PARAM,  0.f, 1.f, 0.1f, "Attack time");
		configParam(RELEASE_PARAM, 0.f, 1.f, 0.3f, "Release time");
		configParam(VARY_PARAM,    0.f, 1.f, 0.f,  "Timing variation");
		configButton(TRIG1_BUTTON, "Trigger Env 1");

		configInput(F1_INPUT, "F1 trigger 1");
		configInput(F3_INPUT, "F3 trigger 2");
		configOutput(F2_OUTPUT, "F2 envelope 1");
		configOutput(F4_OUTPUT, "F4 envelope 2");
	}

	void onReset() override {
		env1.reset();
		env2.reset();
	}

	void process(const ProcessArgs& args) override {
		// Read knobs (0..1, same domain as firmware's adc/1023).
		const float atkPot = params[ATTACK_PARAM].getValue();
		const float relPot = params[RELEASE_PARAM].getValue();
		const float varPot = params[VARY_PARAM].getValue();

		// Convert to time in seconds via shared mapping.
		const float baseAtk = sc::adEnvMapTime(atkPot);
		const float baseRel = sc::adEnvMapTime(relPot);

		// Variation amounts — same formula as firmware.
		const float atkVarAmount = varPot * sc::adEnvVarScale(atkPot);
		const float relVarAmount = varPot * sc::adEnvVarScale(relPot);

		// Trigger 1: jack or front-panel button.
		const bool trig1Jack   = trig1In.process(inputs[F1_INPUT].getVoltage(), 0.1f, 1.f);
		const bool trig1Btn    = trig1Button.process(params[TRIG1_BUTTON].getValue() > 0.5f);
		if (trig1Jack || trig1Btn) {
			const float devAtk = random::uniform() * 2.f - 1.f;
			const float devRel = random::uniform() * 2.f - 1.f;
			env1.trigger(sc::adEnvApplyVariation(baseAtk, devAtk, atkVarAmount),
			             sc::adEnvApplyVariation(baseRel, devRel, relVarAmount));
		}

		// Trigger 2: jack only.
		if (trig2In.process(inputs[F3_INPUT].getVoltage(), 0.1f, 1.f)) {
			const float devAtk = random::uniform() * 2.f - 1.f;
			const float devRel = random::uniform() * 2.f - 1.f;
			env2.trigger(sc::adEnvApplyVariation(baseAtk, devAtk, atkVarAmount),
			             sc::adEnvApplyVariation(baseRel, devRel, relVarAmount));
		}

		// Advance envelopes by one sample.
		const float out1 = env1.process(args.sampleTime);
		const float out2 = env2.process(args.sampleTime);

		// Scale 0..1 → 0..10 V (standard Eurorack envelope range).
		outputs[F2_OUTPUT].setVoltage(out1 * 10.f);
		outputs[F4_OUTPUT].setVoltage(out2 * 10.f);

		// LED tracks Envelope 1 (mirrors firmware's LED on env1.output).
		lights[ENV1_LIGHT].setBrightness(out1);
	}
};

struct DualADEnvWidget : ModuleWidget {
	DualADEnvWidget(DualADEnv* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/DualADEnv.svg")));
		// 4 HP Mod1/Mod2 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.03f, 21.7f)), module, DualADEnv::ATTACK_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 40.06f)), module, DualADEnv::RELEASE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 58.42f)), module, DualADEnv::VARY_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, DualADEnv::TRIG1_BUTTON));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, DualADEnv::ENV1_LIGHT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, DualADEnv::F1_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.3f)), module, DualADEnv::F2_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, DualADEnv::F3_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, DualADEnv::F4_OUTPUT));
	}
};

Model* modelDualADEnv = createModel<DualADEnv, DualADEnvWidget>("DualADEnv");
