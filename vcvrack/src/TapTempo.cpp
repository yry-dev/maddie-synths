#include "plugin.hpp"
#include <TapTempoCore.h>  // Shared tap-tempo timing core (also used by mod1-tap-tempo firmware)

/*
	TapTempo — 4-output tap-tempo master clock.

	Port of firmwares/mod1-tap-tempo/mod1-tap-tempo.ino (Hagiwo Mod1).

	Tap the button (or feed an external clock) at least four times to set the
	tempo; the engine then drives four clock outputs derived from the master 1x
	period:
		POT1 (MULT) -> F2 multiply rate  (1..16x, faster than the beat)
		POT2 (DIV1) -> F3 divide rate    (1..1/16, period expansion)
		POT3 (DIV2) -> F4 divide rate    (1..1/16, period expansion)
		BUTTON      -> tap tempo
		CLOCK in    -> external tap (rising edge), summed with the button
		LED         -> blinks on the master 1x downbeat
		F1          -> fixed 4x clock
		F2          -> multiply clock
		F3 / F4     -> divide / expansion clocks

	Timing translation: the firmware measured tap spacing with millis() and
	scheduled outputs by comparing millisecond timestamps. The shared
	TapTempoCore works in SECONDS and is dt-driven — it accumulates a phase per
	output and emits a rising edge when an output should fire, so the tempo is
	identical at any sample rate. Rack advances it by args.sampleTime; the
	firmware by millis()/1000. Pulse width is owned here by a dsp::PulseGenerator
	per output (the firmware uses a 5ms digitalWrite instead).
*/

struct TapTempo : Module {
	enum ParamId {
		MULT_PARAM,
		DIV1_PARAM,
		DIV2_PARAM,
		TAP_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		CLOCK_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		F1_OUTPUT,
		F2_OUTPUT,
		F3_OUTPUT,
		F4_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		BEAT_LIGHT,
		LIGHTS_LEN
	};

	// Timing state lives in the shared core (same defaults as the firmware).
	sc::TapTempoCore clockCore;

	dsp::BooleanTrigger tapButton;
	dsp::SchmittTrigger clockInput;

	// Pulse generators give each output a clean fixed-width pulse (1ms), the
	// Rack analogue of the firmware's 5ms digitalWrite window.
	dsp::PulseGenerator pulseF1, pulseF2, pulseF3, pulseF4;
	dsp::PulseGenerator beatPulse;  // LED blink on the downbeat

	TapTempo() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		// Knobs are normalized 0..1 and mapped to {1,2,3,4,8,16} in the core.
		configParam(MULT_PARAM, 0.f, 1.f, 0.f, "F2 multiply rate (1..16x)");
		configParam(DIV1_PARAM, 0.f, 1.f, 0.f, "F3 divide rate (1..1/16)");
		configParam(DIV2_PARAM, 0.f, 1.f, 0.f, "F4 divide rate (1..1/16)");
		configButton(TAP_PARAM, "Tap tempo");

		configInput(CLOCK_INPUT, "Clock / tap");
		configOutput(F1_OUTPUT, "F1 4x clock");
		configOutput(F2_OUTPUT, "F2 multiply clock");
		configOutput(F3_OUTPUT, "F3 divide clock");
		configOutput(F4_OUTPUT, "F4 divide clock");
	}

	void onReset() override {
		clockCore.reset();
	}

	void process(const ProcessArgs& args) override {
		// Tap sources: panel button and external clock rising edges are summed.
		bool tapEdge = false;
		if (tapButton.process(params[TAP_PARAM].getValue() > 0.5f))
			tapEdge = true;
		if (clockInput.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f))
			tapEdge = true;

		// Advance the shared engine; pots are the same 0..1 domain as adc/1023.
		const sc::TapTempoEdges e = clockCore.process(
			args.sampleTime,
			tapEdge,
			params[MULT_PARAM].getValue(),
			params[DIV1_PARAM].getValue(),
			params[DIV2_PARAM].getValue());

		// Turn trigger edges into clean pulses (~1ms), output 0/10V gates.
		if (e.f1) pulseF1.trigger(1e-3f);
		if (e.f2) pulseF2.trigger(1e-3f);
		if (e.f3) pulseF3.trigger(1e-3f);
		if (e.f4) pulseF4.trigger(1e-3f);

		outputs[F1_OUTPUT].setVoltage(pulseF1.process(args.sampleTime) ? 10.f : 0.f);
		outputs[F2_OUTPUT].setVoltage(pulseF2.process(args.sampleTime) ? 10.f : 0.f);
		outputs[F3_OUTPUT].setVoltage(pulseF3.process(args.sampleTime) ? 10.f : 0.f);
		outputs[F4_OUTPUT].setVoltage(pulseF4.process(args.sampleTime) ? 10.f : 0.f);

		// LED blinks on the master 1x downbeat (firmware: 10ms; here ~40ms so the
		// blink is visible at fast tempos).
		if (e.beat) beatPulse.trigger(0.04f);
		lights[BEAT_LIGHT].setBrightness(beatPulse.process(args.sampleTime) ? 1.f : 0.f);
	}
};

struct TapTempoWidget : ModuleWidget {
	TapTempoWidget(TapTempo* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/TapTempo.svg")));

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		const float cx = 15.24f; // 6 HP center

		// LED (single beat indicator, like the hardware).
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(cx, 18.5f)), module, TapTempo::BEAT_LIGHT));

		// Three division pots.
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(cx, 30.f)), module, TapTempo::MULT_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(cx, 47.f)), module, TapTempo::DIV1_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(cx, 64.f)), module, TapTempo::DIV2_PARAM));

		// Tap button (momentary).
		addParam(createParamCentered<VCVButton>(mm2px(Vec(cx, 82.f)), module, TapTempo::TAP_PARAM));

		// Clock input (centered) then a 2x2 grid of clock outputs.
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(cx, 95.f)), module, TapTempo::CLOCK_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(10.16f, 107.f)), module, TapTempo::F1_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(20.32f, 107.f)), module, TapTempo::F2_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(10.16f, 119.f)), module, TapTempo::F3_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(20.32f, 119.f)), module, TapTempo::F4_OUTPUT));
	}
};

Model* modelTapTempo = createModel<TapTempo, TapTempoWidget>("TapTempo");
