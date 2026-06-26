#include "plugin.hpp"
#include <LogicPairCore.h>  // Shared boolean-logic core (also used by mod1-logic-pair firmware)

/*
	LogicPair — dual boolean-logic gate processor.

	Port of firmwares/mod1-logic-pair/mod1-logic-pair.ino (Hagiwo for Mod1).

	Six logic modes (MODE knob, matching firmware's select6FromAdc):
		0  AND / NAND
		1  OR  / NOR
		2  XOR / XNOR
		3  COMPARE   (A>B gate on F3, B>A gate on F4)
		4  MAX / MIN (analogue passthrough of larger/smaller input, 0..10V)
		5  FLIP-FLOP (T-type, toggled on rising edge per channel)

	This mirrors the Mod1 hardware: 3 pots, 1 PWM LED, and 4 flexible jacks.
		MODE   -> selects one of 6 logic operations
		IN A   -> offset pot for channel A (summed with F1 CV, like firmware POT_A)
		IN B   -> offset pot for channel B (summed with F2 CV, like firmware POT_B)
		F1     -> gate/CV input A (Schmitt trigger, combined with IN A offset)
		F2     -> gate/CV input B (Schmitt trigger, combined with IN B offset)
		F3     -> output A (gate 0/10V for boolean modes; analogue for MAX/MIN)
		F4     -> output B (complement/secondary output)
		LED    -> tracks output A brightness (matches firmware's OCR2B = outA)
*/

struct LogicPair : Module {
	enum ParamId {
		MODE_PARAM,
		OFFSET_A_PARAM,
		OFFSET_B_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		F1_INPUT,   // gate/CV input A
		F2_INPUT,   // gate/CV input B
		INPUTS_LEN
	};
	enum OutputId {
		F3_OUTPUT,  // logic result A
		F4_OUTPUT,  // logic result B
		OUTPUTS_LEN
	};
	enum LightId {
		OUT_A_LIGHT,
		LIGHTS_LEN
	};

	// Logic state lives in the shared core (same initial state as the firmware).
	sc::LogicPairVoice logic;

	// Schmitt triggers for the two gate inputs.
	dsp::SchmittTrigger schmittA;
	dsp::SchmittTrigger schmittB;

	LogicPair() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(MODE_PARAM, 0.f, 1.f, 0.f, "Logic mode (AND/NAND · OR/NOR · XOR/XNOR · COMPARE · MAX/MIN · FLIP-FLOP)");
		configParam(OFFSET_A_PARAM, 0.f, 1.f, 0.f, "Input A offset (summed with F1)");
		configParam(OFFSET_B_PARAM, 0.f, 1.f, 0.f, "Input B offset (summed with F2)");

		configInput(F1_INPUT, "F1 gate/CV A");
		configInput(F2_INPUT, "F2 gate/CV B");

		configOutput(F3_OUTPUT, "F3 output A");
		configOutput(F4_OUTPUT, "F4 output B");
	}

	void onReset() override {
		logic.reset();
	}

	void process(const ProcessArgs& args) override {
		// Map mode knob (0..1) to one of 6 logic operations via shared selector.
		const uint8_t mode = sc::select6(params[MODE_PARAM].getValue());

		// Sum the offset pot (0..1 → 0..10V equivalent) with the CV jack.
		// Mirrors the firmware's addClamp1023(rawPot, rawCV).
		const float combinedA = params[OFFSET_A_PARAM].getValue() * 10.f
		                        + inputs[F1_INPUT].getVoltage();
		const float combinedB = params[OFFSET_B_PARAM].getValue() * 10.f
		                        + inputs[F2_INPUT].getVoltage();

		// The core needs the sustained gate LEVEL, not an edge. SchmittTrigger::
		// process() returns true only on the low->high transition, so read the
		// held state via isHigh() instead. Threshold at the firmware's midpoint
		// (sum > 512 over 0..1023 ≈ 5 V) with a 1 V hysteresis band for noise
		// immunity. (Using process()'s return value here broke AND/OR/XOR.)
		schmittA.process(combinedA, 4.f, 6.f);
		schmittB.process(combinedB, 4.f, 6.f);
		const bool digitalA = schmittA.isHigh();
		const bool digitalB = schmittB.isHigh();

		// Normalised analogue levels (0..1) for COMPARE and MAX/MIN modes.
		const float valA01 = clamp(combinedA / 10.f, 0.f, 1.f);
		const float valB01 = clamp(combinedB / 10.f, 0.f, 1.f);

		// Run the shared logic core.
		const sc::LogicPairResult out = logic.step(digitalA, digitalB,
		                                            valA01, valB01, mode);

		// Scale 0..1 → 0..10V (gate modes snap to 0 or 10V; MAX/MIN is analogue).
		outputs[F3_OUTPUT].setVoltage(out.outA * 10.f);
		outputs[F4_OUTPUT].setVoltage(out.outB * 10.f);

		// LED tracks output A (matches firmware's OCR2B = outA).
		lights[OUT_A_LIGHT].setBrightness(out.outA);
	}
};

struct LogicPairWidget : ModuleWidget {
	LogicPairWidget(LogicPair* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/LogicPair.svg")));
		// 4 HP Mod1/Mod2 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.03f, 21.7f)), module, LogicPair::MODE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 40.06f)), module, LogicPair::OFFSET_A_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 58.42f)), module, LogicPair::OFFSET_B_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, LogicPair::OUT_A_LIGHT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, LogicPair::F1_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.3f)), module, LogicPair::F2_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, LogicPair::F3_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, LogicPair::F4_OUTPUT));
	}
};

Model* modelLogicPair = createModel<LogicPair, LogicPairWidget>("LogicPair");
