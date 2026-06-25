#include "plugin.hpp"
#include <LorenzVoice.h>  // Shared Lorenz core (also used by mod1-butterfly firmware)

/*
	Butterfly — Lorenz attractor chaotic CV generator.

	Port of firmwares/mod1-butterfly/mod1-butterfly.ino (Rob Heel, for Hagiwo Mod1).

	The Lorenz system:
		dx = sigma * (y - x)
		dy = x * (rho - z) - y
		dz = x * y - beta * z

	This mirrors the Mod1 hardware exactly: 3 pots, 1 push button, 1 PWM LED, and
	4 flexible jacks (F1..F4). For the Butterfly firmware:
		POT1  -> Sigma (also picks one of 3 integration step sizes)
		POT2  -> Rho
		POT3  -> Beta
		BUTTON-> toggles slow mode (persists)
		LED   -> blinks at the integration step rate
		F1    -> trigger input (resets the attractor on a rising edge)
		F2    -> X axis output
		F3    -> Y axis output
		F4    -> Z axis output

	On the Nano the integration rate came from the Arduino loop speed times the
	per-step dt. Rack runs at audio rate, so we emulate the loop with LOOP_HZ and
	advance simulation time accordingly, sub-stepped at a fixed internal dt so Euler
	integration stays stable and sample-rate independent.
*/

// Emulated Arduino loop rate (the Nano loop is ~a couple kHz with its analogReads).
// This sets how fast the attractor evolves for a given step size.
static const float LOOP_HZ = 2000.f;

struct Butterfly : Module {
	enum ParamId {
		SIGMA_PARAM,
		RHO_PARAM,
		BETA_PARAM,
		SLOW_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		F1_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		F2_OUTPUT,
		F3_OUTPUT,
		F4_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		STEP_LIGHT,
		LIGHTS_LEN
	};

	// Lorenz state lives in the shared core (same seed as the firmware).
	sc::LorenzVoice lorenz;

	// LED blink state.
	float blinkPhase = 0.f;
	bool ledOn = false;

	dsp::SchmittTrigger resetTrigger;

	Butterfly() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		// Knobs are normalized 0..1 and mapped to the firmware's ranges in process().
		configParam(SIGMA_PARAM, 0.f, 1.f, 0.5f, "Sigma (flow strength + step size)");
		configParam(RHO_PARAM, 0.f, 1.f, 0.5f, "Rho (divergence / chaos)");
		configParam(BETA_PARAM, 0.f, 1.f, 0.5f, "Beta (damping)");
		configSwitch(SLOW_PARAM, 0.f, 1.f, 0.f, "Slow mode", {"Off", "On"});

		configInput(F1_INPUT, "F1 reset trigger");
		configOutput(F2_OUTPUT, "F2 X axis");
		configOutput(F3_OUTPUT, "F3 Y axis");
		configOutput(F4_OUTPUT, "F4 Z axis");
	}

	void onReset() override {
		lorenz.reset();
	}

	void process(const ProcessArgs& args) override {
		const bool slow = params[SLOW_PARAM].getValue() > 0.5f;

		// F1: reset attractor on a rising edge (>1V), like the trigger input.
		if (resetTrigger.process(inputs[F1_INPUT].getVoltage(), 0.1f, 1.f))
			lorenz.reset();

		// Map pots (0..1, same domain as the firmware's adc/1023) to Lorenz
		// parameters and the integration step size, via the shared core.
		const sc::LorenzParams lp = sc::lorenzMapParams(
			params[SIGMA_PARAM].getValue(),
			params[RHO_PARAM].getValue(),
			params[BETA_PARAM].getValue(),
			slow);
		const float fwDt = lp.dt;

		// Advance simulation time for this sample, emulating LOOP_HZ steps of fwDt
		// per second. Sub-step at a fixed internal dt for numeric stability.
		const float simDelta = fwDt * LOOP_HZ * args.sampleTime;
		const float maxDt = 0.004f;
		int steps = clamp((int) std::ceil(simDelta / maxDt), 1, 64);
		const float dt = simDelta / steps;

		for (int i = 0; i < steps; i++)
			lorenz.step(lp.sigma, lp.rho, lp.beta, dt);

		// Scale to Rack voltages. x/y are naturally bipolar (~±30) -> ±5V.
		// z is unipolar (~0..50) -> 0..10V. Same source ranges as the firmware.
		outputs[F2_OUTPUT].setVoltage(clamp(lorenz.x / 30.f * 5.f, -5.f, 5.f));
		outputs[F3_OUTPUT].setVoltage(clamp(lorenz.y / 30.f * 5.f, -5.f, 5.f));
		outputs[F4_OUTPUT].setVoltage(clamp(lorenz.z / 50.f * 10.f, 0.f, 10.f));

		// LED blinks at the step rate (firmware: faster step -> faster blink).
		const float blinkMs = clamp(rescale(fwDt * 1e6f, 50.f, 10000.f, 500.f, 50.f), 50.f, 500.f);
		blinkPhase += args.sampleTime;
		if (blinkPhase >= blinkMs * 1e-3f) {
			blinkPhase = 0.f;
			ledOn = !ledOn;
		}
		lights[STEP_LIGHT].setBrightness(ledOn ? 1.f : 0.f);
	}
};

struct ButterflyWidget : ModuleWidget {
	ButterflyWidget(Butterfly* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Butterfly.svg")));
		// 4 HP Mod1/Mod2 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.03f, 21.7f)), module, Butterfly::SIGMA_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 40.06f)), module, Butterfly::RHO_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 58.42f)), module, Butterfly::BETA_PARAM));
		addParam(createParamCentered<VCVLatch>(mm2px(Vec(5.19f, 78.57f)), module, Butterfly::SLOW_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, Butterfly::STEP_LIGHT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, Butterfly::F1_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.3f)), module, Butterfly::F2_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, Butterfly::F3_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, Butterfly::F4_OUTPUT));
	}
};

Model* modelButterfly = createModel<Butterfly, ButterflyWidget>("Butterfly");
