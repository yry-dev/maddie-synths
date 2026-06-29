#include "plugin.hpp"
#include <RandomCvCore.h>  // Shared random CV sequencer core (also used by mod1-random-cv firmware)

/*
	RandomCV — Periodic random CV sequencer with gate output.

	Port of firmwares/mod1-random-cv/mod1-random-cv.ino (for Hagiwo Mod1).

	A cyclic pattern of random CV and gate values with selectable length
	(3/4/5/8/16/32 steps), re-randomised via front-panel button or F2 trigger
	input. Advances one step per rising clock edge on F1.

		POT1  -> Step count (3, 4, 5, 8, 16, 32 via select6 thresholds)
		POT2  -> CV level (output amplitude, 0..10V)
		POT3  -> Gate probability (0..100%)
		BTN   -> Re-randomize pattern (momentary)
		LED   -> CV brightness (proportional to current CV value)
		F1    -> Clock input (advances one step per rising edge)
		F2    -> Reseed trigger input (re-randomize on rising edge)
		F3    -> CV output (0..10V, level-scaled)
		F4    -> Gate output (10V pulse, 2 ms, matches firmware pulse width)

	Behavior note: the gate fires when the stored trigger value for the current
	step is less than the probability threshold, matching the firmware comparison
	trigValues[step] < map(pot, 0, 1023, 0, 255). At maximum probability a
	slot holding value 255 will not fire (1-in-256 chance); this mirrors the
	firmware exactly.
*/

struct RandomCV : Module {
	enum ParamId {
		STEPS_PARAM,
		LEVEL_PARAM,
		PROB_PARAM,
		RESEED_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		CLOCK_INPUT,   // F1 — clock
		RESEED_INPUT,  // F2 — reseed trigger
		INPUTS_LEN
	};
	enum OutputId {
		CV_OUTPUT,    // F3 — CV (0..10V)
		GATE_OUTPUT,  // F4 — gate (10V, 2 ms)
		OUTPUTS_LEN
	};
	enum LightId {
		CV_LIGHT,
		LIGHTS_LEN
	};

	sc::RandomCvVoice voice;
	dsp::SchmittTrigger clockTrig;
	dsp::SchmittTrigger reseedInputTrig;
	dsp::BooleanTrigger reseedParamTrig;
	dsp::PulseGenerator gatePulse;

	RandomCV() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(STEPS_PARAM,  0.f, 1.f, 0.5f, "Steps (3, 4, 5, 8, 16, 32)");
		configParam(LEVEL_PARAM,  0.f, 1.f, 1.0f, "CV Level");
		configParam(PROB_PARAM,   0.f, 1.f, 0.5f, "Gate Probability");
		configButton(RESEED_PARAM, "Reseed pattern");
		configInput(CLOCK_INPUT,  "Clock");
		configInput(RESEED_INPUT, "Reseed trigger");
		configOutput(CV_OUTPUT,   "CV (0..10V)");
		configOutput(GATE_OUTPUT, "Gate (10V, 2 ms)");
	}

	void onReset() override {
		voice.seed(0xDEADBEEFu);
	}

	void process(const ProcessArgs& args) override {
		const sc::RandomCvParams p = sc::randomCvMapParams(
			params[STEPS_PARAM].getValue(),
			params[LEVEL_PARAM].getValue(),
			params[PROB_PARAM].getValue());

		// Reseed: rising edge on F2 input or front-panel button press.
		if (reseedInputTrig.process(inputs[RESEED_INPUT].getVoltage(), 0.1f, 1.f))
			voice.randomize();
		if (reseedParamTrig.process(params[RESEED_PARAM].getValue() > 0.5f))
			voice.randomize();

		// Clock: advance the sequencer on a rising edge.
		const bool clockRose = clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f);
		const sc::RandomCvFrame frame = voice.step(clockRose, p);

		// Gate: 2 ms pulse on steps that fire (matches firmware 2 ms pulse width).
		if (clockRose && frame.gate)
			gatePulse.trigger(2e-3f);

		outputs[CV_OUTPUT].setVoltage(frame.cv * 10.f);
		outputs[GATE_OUTPUT].setVoltage(gatePulse.process(args.sampleTime) ? 10.f : 0.f);

		// LED brightness tracks the current CV output level.
		lights[CV_LIGHT].setBrightness(frame.cv);
	}
};

struct RandomCVWidget : ModuleWidget {
	RandomCVWidget(RandomCV* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/mod1-random-cv.svg")));
		// 4 HP Mod1/Mod2 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.03f, 21.7f)), module, RandomCV::STEPS_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 40.06f)), module, RandomCV::LEVEL_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 58.42f)), module, RandomCV::PROB_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, RandomCV::RESEED_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, RandomCV::CV_LIGHT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, RandomCV::CLOCK_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.3f)), module, RandomCV::RESEED_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, RandomCV::CV_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, RandomCV::GATE_OUTPUT));
	}
};

Model* modelRandomCV = createModel<RandomCV, RandomCVWidget>("mod1-random-cv");
