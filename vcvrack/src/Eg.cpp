#include "plugin.hpp"
#include <EgCore.h>  // Shared EG core (also used by mod1-eg firmware)

/*
	EG — 3-output AR envelope generator with end-of-cycle gate.

	Port of firmwares/mod1-eg/mod1-eg.ino (Hagiwo Mod1).

	The envelope is a two-segment AR shape using an exponential curve
	(expf(−3×phase)) that replaces the firmware's 1024-entry PROGMEM table.
	Attack rises from the retrigger level to full; release falls to zero.
	An EoC (end-of-cycle) gate fires for 10 ms at the end of each release,
	which can be self-patched to F1 for LFO / clock use.

	Jack assignments (mirror the firmware):
		POT1   -> Attack time    (short to long, ~40 ms – 41 s)
		POT2   -> Release time   (short to long, ~40 ms – 41 s)
		POT3   -> Output level   (0..1 attenuation)
		BUTTON -> Manual trigger (momentary)
		LED    -> Envelope brightness
		F1     -> Trigger input (rising edge)
		F2     -> End-of-cycle gate (10 ms pulse, 0 / 10 V)
		F3     -> Inverted EG (10 V − EG)
		F4     -> EG output (0..10 V)
*/

struct Eg : Module {
	enum ParamId {
		ATTACK_PARAM,
		RELEASE_PARAM,
		LEVEL_PARAM,
		TRIG_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		F1_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		F2_OUTPUT,   // EoC gate
		F3_OUTPUT,   // inverted EG
		F4_OUTPUT,   // EG
		OUTPUTS_LEN
	};
	enum LightId {
		ENV_LIGHT,
		LIGHTS_LEN
	};

	// Envelope state lives in the shared core (same algorithm as the firmware).
	sc::EgVoice eg;

	dsp::SchmittTrigger inTrig;
	dsp::BooleanTrigger btnTrig;

	Eg() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(ATTACK_PARAM,  0.f, 1.f, 0.5f, "Attack time");
		configParam(RELEASE_PARAM, 0.f, 1.f, 0.5f, "Release time");
		configParam(LEVEL_PARAM,   0.f, 1.f, 1.0f, "Output level");
		configButton(TRIG_PARAM, "Manual trigger");

		configInput(F1_INPUT,  "F1 trigger");
		configOutput(F2_OUTPUT, "F2 end-of-cycle gate");
		configOutput(F3_OUTPUT, "F3 inverted EG");
		configOutput(F4_OUTPUT, "F4 EG");
	}

	void onReset() override {
		eg.reset();
	}

	void process(const ProcessArgs& args) override {
		// Map pots (0..1) to envelope parameters via the shared core.
		const sc::EgParams ep = sc::egMapParams(
			params[ATTACK_PARAM].getValue(),
			params[RELEASE_PARAM].getValue(),
			params[LEVEL_PARAM].getValue()
		);

		// Fire on F1 rising edge (>1 V) or manual button press.
		const bool gate   = inTrig.process(inputs[F1_INPUT].getVoltage(), 0.1f, 1.f);
		const bool button = btnTrig.process(params[TRIG_PARAM].getValue() > 0.5f);
		if (gate || button)
			eg.trigger();

		// Advance the envelope by one sample.
		const float env = eg.process(args.sampleTime, ep);

		// Scale to Rack voltages. EG: 0..1 -> 0..10 V. Inv EG: mirrored.
		outputs[F4_OUTPUT].setVoltage(env * 10.f);
		outputs[F3_OUTPUT].setVoltage((1.f - env) * 10.f);
		outputs[F2_OUTPUT].setVoltage(eg.eoc ? 10.f : 0.f);

		// LED brightness tracks the envelope level.
		lights[ENV_LIGHT].setBrightness(env);
	}
};

struct EgWidget : ModuleWidget {
	EgWidget(Eg* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Eg.svg")));
		// 4 HP Mod1/Mod2 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.03f, 21.7f)), module, Eg::ATTACK_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 40.06f)), module, Eg::RELEASE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 58.42f)), module, Eg::LEVEL_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, Eg::TRIG_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, Eg::ENV_LIGHT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, Eg::F1_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.3f)), module, Eg::F2_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, Eg::F3_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, Eg::F4_OUTPUT));
	}
};

Model* modelEg = createModel<Eg, EgWidget>("Eg");
