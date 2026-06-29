#include "plugin.hpp"
#include <TerrainLfoCore.h>  // Shared terrain core (also used by mod1-terrain-lfo firmware)

/*
	TerrainLFO — procedurally generated triple-wavetable chaotic LFO.

	Port of firmwares/mod1-terrain-lfo/mod1-terrain-lfo.ino (Rob Heel, for Hagiwo Mod1).

	On each GEN press three independent "terrain" wavetables are generated from a
	handful of random knots (seamless loop, a guaranteed zero and peak, nonlinear
	knot spacing, one Bezier-curved segment). The three terrains are read back at
	a fixed detune (x0.9 / x1.0 / x1.1) and each can independently drop into a
	tempo-scaled "SloMo breath". This mirrors the Mod1 hardware: 3 pots, 1 push
	button, 1 LED, 4 flexible jacks.
		POT1  -> SPEED (base read speed, 0.01 .. ~3 Hz)
		POT2  -> SLOMO (per-step SloMo trigger probability / intensity)
		POT3  -> KNOTS (knots per waveform, 3..12; sampled on regenerate)
		BUTTON-> GEN   (regenerate all three terrains)
		LED   -> lit briefly while generating
		F1    -> speed CV input (0..5V adds 0..1 Hz)
		F2/F3/F4 -> the three detuned terrain outputs (0..10V)

	On the Nano the engine advanced once per Arduino loop iteration. The terrain
	read-speed is purely time-driven (loop-rate independent), but the 0.9/0.1
	output smoothing and the SloMo trigger odds are *per-iteration* quantities, so
	we emulate the loop at LOOP_HZ and run whole loop iterations (accumulating
	fractional ones), rather than sub-dividing a single iteration as the Lorenz
	port can. dt is fixed at 1/LOOP_HZ for stability and sample-rate independence.
*/

// Emulated Arduino loop rate. The Nano loop (3 analogReads + pow + 3 table reads)
// runs at roughly this rate; it sets the output smoothing cutoff and the SloMo
// event rate (the read speed itself is independent of it).
static const float LOOP_HZ = 1000.f;

struct TerrainLFO : Module {
	enum ParamId {
		SPEED_PARAM,
		SLOMO_PARAM,
		KNOTS_PARAM,
		GEN_PARAM,
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
		GEN_LIGHT,
		LIGHTS_LEN
	};

	// Terrain state lives in the shared core (same engine as the firmware).
	sc::TerrainLfoCore core;

	// Fractional loop-iteration accumulator and generate-LED timer.
	float loopAccum = 0.f;
	float blinkRemaining = 0.f;

	dsp::SchmittTrigger genTrigger;

	TerrainLFO() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		// Knobs are normalized 0..1 and mapped to the firmware's ranges in process().
		configParam(SPEED_PARAM, 0.f, 1.f, 0.5f, "Base speed");
		configParam(SLOMO_PARAM, 0.f, 1.f, 0.f, "SloMo probability");
		configParam(KNOTS_PARAM, 0.f, 1.f, 0.5f, "Knots per waveform");
		configButton(GEN_PARAM, "Generate new terrains");

		configInput(F1_INPUT, "F1 speed CV (0..5V -> 0..1 Hz)");
		configOutput(F2_OUTPUT, "F2 terrain 1");
		configOutput(F3_OUTPUT, "F3 terrain 2");
		configOutput(F4_OUTPUT, "F4 terrain 3");

		// Build an initial set of terrains so the module sounds on load.
		core.regenerate((int) rescale(0.5f, 0.f, 1.f, 3.f, 12.f), random::u32());
	}

	void onReset() override {
		core.reset();
		core.regenerate(
			(int) clamp(rescale(params[KNOTS_PARAM].getValue(), 0.f, 1.f, 3.f, 12.f), 3.f, 12.f),
			random::u32());
	}

	void process(const ProcessArgs& args) override {
		const sc::TerrainParams tp = sc::terrainMapParams(
			params[SPEED_PARAM].getValue(),
			params[SLOMO_PARAM].getValue(),
			params[KNOTS_PARAM].getValue());

		// GEN button: regenerate all three terrains and flash the LED.
		if (genTrigger.process(params[GEN_PARAM].getValue())) {
			core.regenerate(tp.knots, random::u32());
			blinkRemaining = 0.2f; // 200 ms, like the firmware's blinkDuration
		}

		// F1: speed CV, 0..5V -> 0..1 Hz offset (same as the firmware's ADC map).
		const float cvHz = clamp(inputs[F1_INPUT].getVoltage(), 0.f, 5.f) / 5.f;

		// Emulate whole loop iterations at LOOP_HZ, accumulating fractional ones.
		loopAccum += LOOP_HZ * args.sampleTime;
		int iters = (int) loopAccum;
		loopAccum -= (float) iters;
		iters = clamp(iters, 0, 64);
		const float fwDt = 1.f / LOOP_HZ;
		for (int i = 0; i < iters; i++)
			core.step(fwDt, tp.baseHz, cvHz, tp.intensity);

		// The terrains are unipolar (0..1) -> 0..10V, same source range as the
		// firmware's 0..255 PWM outputs.
		outputs[F2_OUTPUT].setVoltage(clamp(core.out[0] * 10.f, 0.f, 10.f));
		outputs[F3_OUTPUT].setVoltage(clamp(core.out[1] * 10.f, 0.f, 10.f));
		outputs[F4_OUTPUT].setVoltage(clamp(core.out[2] * 10.f, 0.f, 10.f));

		// LED lit while generating.
		if (blinkRemaining > 0.f) blinkRemaining -= args.sampleTime;
		lights[GEN_LIGHT].setBrightness(blinkRemaining > 0.f ? 1.f : 0.f);
	}
};

struct TerrainLFOWidget : ModuleWidget {
	TerrainLFOWidget(TerrainLFO* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/mod1-terrain-lfo.svg")));
		// 4 HP Mod1/Mod2 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.03f, 21.7f)), module, TerrainLFO::SPEED_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 40.06f)), module, TerrainLFO::SLOMO_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 58.42f)), module, TerrainLFO::KNOTS_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, TerrainLFO::GEN_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, TerrainLFO::GEN_LIGHT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, TerrainLFO::F1_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.3f)), module, TerrainLFO::F2_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, TerrainLFO::F3_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, TerrainLFO::F4_OUTPUT));
	}
};

Model* modelTerrainLFO = createModel<TerrainLFO, TerrainLFOWidget>("mod1-terrain-lfo");
