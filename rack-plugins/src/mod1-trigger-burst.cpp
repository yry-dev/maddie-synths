#include "plugin.hpp"
#include <TriggerBurstCore.h>  // Shared burst core (also used by mod1-trigger-burst firmware)

/*
	TriggerBurst — clock-syncable trigger burst generator.

	Port of firmwares/mod1-trigger-burst/mod1-trigger-burst.ino (Hagiwo Mod1).

	On a rising edge at the TRIG input (F2) or a press of the BURST button,
	the module fires N trigger pulses at an interval derived from the clock
	(internal BPM or external clock at F1), divided by the DIV ratio.

	Panel layout mirrors the Mod1 hardware exactly:
		NUM   (POT1+F3 CV) — burst count: 1, 3, 4, 6, 8, 16
		DIV   (POT2)       — clock division: /2, /3, /4, /6, /8, /16
		CLOCK (POT3)       — internal BPM (80..280); fully left → use F1 ext clock
		BURST (BTN)        — manual trigger (momentary)
		LED                — mirrors trigger output
		F1    IN           — external clock input
		F2    IN           — trigger input
		F3    IN           — burst-number CV (summed with NUM knob, clamped 0..1)
		F4    OUT          — trigger/gate output
*/

// External clock threshold: matches firmware's `potValue < 50` out of 1023.
static const float kExtClkThreshold = 50.0f / 1023.0f;

struct TriggerBurst : Module {
	enum ParamId {
		NUM_PARAM,
		DIV_PARAM,
		CLOCK_PARAM,
		MANUAL_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		F1_INPUT,   // external clock
		F2_INPUT,   // trigger in
		F3_INPUT,   // burst-number CV
		INPUTS_LEN
	};
	enum OutputId {
		F4_OUTPUT,  // trigger/gate out
		OUTPUTS_LEN
	};
	enum LightId {
		GATE_LIGHT,
		LIGHTS_LEN
	};

	sc::TriggerBurstVoice burst;

	dsp::SchmittTrigger trigIn;
	dsp::SchmittTrigger extClkIn;
	dsp::BooleanTrigger manualBtn;

	// External clock period tracking: average of last 3 intervals (matches firmware)
	float extClkPeriods[3] = {0.f, 0.f, 0.f};
	int   extClkIdx        = 0;
	float lastExtClkTime   = 0.f;   // seconds since module start
	float extClkPeriodSec  = 0.f;
	float runTime          = 0.f;   // accumulated seconds

	TriggerBurst() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(NUM_PARAM,    0.f, 1.f, 0.5f, "Burst count", "", 0.f, 1.f);
		configParam(DIV_PARAM,    0.f, 1.f, 0.5f, "Clock division");
		configParam(CLOCK_PARAM,  0.f, 1.f, 0.5f, "Internal BPM / ext clock select");
		configButton(MANUAL_PARAM, "Manual burst");

		configInput(F1_INPUT, "F1 external clock");
		configInput(F2_INPUT, "F2 trigger");
		configInput(F3_INPUT, "F3 burst-count CV");
		configOutput(F4_OUTPUT, "F4 trigger/gate");
	}

	void onReset() override {
		burst.reset();
		extClkPeriods[0] = extClkPeriods[1] = extClkPeriods[2] = 0.f;
		extClkIdx       = 0;
		lastExtClkTime  = runTime;
		extClkPeriodSec = 0.f;
	}

	void process(const ProcessArgs& args) override {
		runTime += args.sampleTime;

		// --- External clock period measurement (mirrors firmware's checkExternalClock) ---
		if (extClkIn.process(inputs[F1_INPUT].getVoltage(), 0.1f, 1.f)) {
			float period = runTime - lastExtClkTime;
			lastExtClkTime = runTime;
			if (period > 0.f) {
				extClkPeriods[extClkIdx] = period;
				extClkIdx = (extClkIdx + 1) % 3;
				float sum = extClkPeriods[0] + extClkPeriods[1] + extClkPeriods[2];
				extClkPeriodSec = sum / 3.f;
			}
		}

		// --- Parameter mapping ---
		// NUM knob (0..1) + F3 CV (0..5V scaled to 0..1), summed and clamped — mirrors
		// firmware's addClamp1023(analogRead(POT1), analogRead(CV3)) / 1023.f
		float numNorm = clamp(
			params[NUM_PARAM].getValue() + inputs[F3_INPUT].getVoltage() / 5.f,
			0.f, 1.f);
		float divNorm   = params[DIV_PARAM].getValue();
		float clockNorm = params[CLOCK_PARAM].getValue();

		sc::TriggerBurstParams p = sc::triggerBurstMapParams(numNorm, divNorm, clockNorm);

		// --- Clock source (mirrors firmware's potValue < 50 threshold) ---
		bool useExternal = (clockNorm < kExtClkThreshold);
		float clockPeriodSec;
		if (useExternal && extClkPeriodSec > 0.f) {
			clockPeriodSec = extClkPeriodSec;
		} else {
			clockPeriodSec = 60.f / p.bpm;
		}

		// --- Trigger detection ---
		bool trigRose = trigIn.process(inputs[F2_INPUT].getVoltage(), 0.1f, 1.f);
		bool btnPress = manualBtn.process(params[MANUAL_PARAM].getValue() > 0.5f);

		// --- Advance burst core ---
		sc::TriggerBurstResult r = burst.process(
			args.sampleTime, trigRose || btnPress,
			p.numTriggers, p.divRatio, clockPeriodSec
		);

		// --- Outputs ---
		outputs[F4_OUTPUT].setVoltage(r.gateOn ? 10.f : 0.f);
		lights[GATE_LIGHT].setBrightness(r.gateOn ? 1.f : 0.f);
	}
};

struct TriggerBurstWidget : ModuleWidget {
	TriggerBurstWidget(TriggerBurst* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/mod1-trigger-burst.svg")));
		// 4 HP Mod1/Mod2 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.03f, 21.7f)), module, TriggerBurst::NUM_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 40.06f)), module, TriggerBurst::DIV_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 58.42f)), module, TriggerBurst::CLOCK_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, TriggerBurst::MANUAL_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, TriggerBurst::GATE_LIGHT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, TriggerBurst::F1_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.3f)), module, TriggerBurst::F2_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, TriggerBurst::F3_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, TriggerBurst::F4_OUTPUT));
	}
};

Model* modelTriggerBurst = createModel<TriggerBurst, TriggerBurstWidget>("mod1-trigger-burst");
