#include "plugin.hpp"
#include <EuclideanCore.h>  // Shared Euclidean sequencer core (also used by mod1-euclidean firmware)

/*
	Euclidean — 8/16-step Euclidean rhythm sequencer with probability gating.

	Port of firmwares/mod1-euclidean/mod1-euclidean.ino (Hagiwo for Mod1).

	Euclidean patterns are generated on-the-fly via the Bjorklund/Bresenham
	formula in EuclideanCore.h — no lookup tables. Matches the original
	firmware's PROGMEM tables for every (k, n) in {0..8}×8 and {0..16}×16.

	Panel layout mirrors Mod1 hardware:
		HITS knob     -> number of hits in the pattern (A0)
		PROB knob     -> trigger probability 0..100% (A1)
		LENGTH latch  -> 8-step (off) or 16-step (on) mode (A2 > 0.5)
		RESET button  -> reset step to 0 (mirrors BTN + F1)
		F1 (IN)       -> reset trigger input
		F2 (IN)       -> clock input
		F3 (IN)       -> hits CV (0..5 V adds to HITS knob, clamped to 0..1)
		F4 (OUT)      -> gate output, 10 V trigger pulse (~10 ms, matches firmware)
		LED           -> illuminates for the duration of each gate pulse
*/

struct Euclidean : Module {
	enum ParamId {
		HITS_PARAM,
		PROB_PARAM,
		LENGTH_PARAM,
		RESET_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		F1_RESET_INPUT,
		F2_CLOCK_INPUT,
		F3_HITSCV_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		F4_GATE_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		STEP_LIGHT,
		LIGHTS_LEN
	};

	// Sequencer state lives in the shared core (same initial step as firmware).
	sc::EuclideanVoice euclid;

	dsp::SchmittTrigger resetTrigger;
	dsp::SchmittTrigger clockTrigger;
	dsp::BooleanTrigger resetBtnTrigger;
	dsp::PulseGenerator gatePulse;

	Euclidean() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(HITS_PARAM,    0.f, 1.f, 0.5f, "Hits");
		configParam(PROB_PARAM,    0.f, 1.f, 1.0f, "Probability");
		configSwitch(LENGTH_PARAM, 0.f, 1.f, 0.f,  "Step length", {"8 steps", "16 steps"});
		configParam(RESET_PARAM,   0.f, 1.f, 0.f,  "Reset step");

		configInput(F1_RESET_INPUT,  "F1 reset trigger");
		configInput(F2_CLOCK_INPUT,  "F2 clock");
		configInput(F3_HITSCV_INPUT, "F3 hits CV");
		configOutput(F4_GATE_OUTPUT, "F4 gate");
	}

	void onReset() override {
		euclid.reset();
	}

	void process(const ProcessArgs& args) override {
		// Reset step on rising edge of F1 input or RESET button.
		if (resetTrigger.process(inputs[F1_RESET_INPUT].getVoltage(), 0.1f, 1.f)
			|| resetBtnTrigger.process(params[RESET_PARAM].getValue() > 0.5f))
			euclid.reset();

		// Clock: advance sequencer on rising edge of F2.
		const bool clockRose = clockTrigger.process(inputs[F2_CLOCK_INPUT].getVoltage(), 0.1f, 1.f);

		// Hits: pot + optional F3 CV (0..5 V adds to the 0..1 pot value).
		// Mirrors firmware's min(analogRead(potPin)+5 + analogRead(hitCVPin), 1023).
		float hits01 = params[HITS_PARAM].getValue();
		if (inputs[F3_HITSCV_INPUT].isConnected())
			hits01 = clamp(hits01 + inputs[F3_HITSCV_INPUT].getVoltage() / 5.f, 0.f, 1.f);

		// Map controls to sequencer parameters via shared core.
		const sc::EuclideanParams ep = sc::euclideanMapParams(
			hits01,
			params[PROB_PARAM].getValue(),
			params[LENGTH_PARAM].getValue());

		// Step the sequencer; draw a fresh random value for the probability gate.
		if (euclid.step(clockRose, random::uniform(), ep))
			gatePulse.trigger(0.01f);  // 10 ms pulse, matches firmware's triggerTime

		const bool gateHigh = gatePulse.process(args.sampleTime);
		outputs[F4_GATE_OUTPUT].setVoltage(gateHigh ? 10.f : 0.f);
		lights[STEP_LIGHT].setBrightness(gateHigh ? 1.f : 0.f);
	}
};

struct EuclideanWidget : ModuleWidget {
	EuclideanWidget(Euclidean* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Euclidean.svg")));
		// 4 HP Mod1/Mod2 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.03f, 21.7f)), module, Euclidean::HITS_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 40.06f)), module, Euclidean::PROB_PARAM));
		addParam(createParamCentered<VCVLatch>(mm2px(Vec(10.04f, 58.42f)), module, Euclidean::LENGTH_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, Euclidean::RESET_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, Euclidean::STEP_LIGHT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, Euclidean::F1_RESET_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.3f)), module, Euclidean::F2_CLOCK_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, Euclidean::F3_HITSCV_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, Euclidean::F4_GATE_OUTPUT));
	}
};

Model* modelEuclidean = createModel<Euclidean, EuclideanWidget>("Euclidean");
