#include "plugin.hpp"
#include <ClavesVoice.h>  // Shared Claves voice (also used by mod2-claves firmware)

/*
	Claves — tunable claves / woodblock percussion voice.

	Port of firmwares/mod2-claves/claves.ino (HAGIWO Mod2, RP2350).

	Mirrors the Mod2 hardware: 3 pots, 1 push button, 1 PWM LED, and the Mod2
	jack set (IN1, IN2, CV, OUT). For the Claves firmware:
		POT1   -> Decay (envelope decay rate, 1..10)
		POT2   -> Wave morph (sine <-> triangle)
		POT3   -> Pitch (shares the CV jack on hardware)
		BUTTON -> manual trigger
		LED    -> envelope brightness
		IN1    -> trigger input (rising edge)
		IN2    -> unused by this firmware
		CV     -> 1V/Oct pitch
		OUT    -> audio output

	The firmware renders a fixed 8192-sample table at ~36.6 kHz (~224 ms) on each
	strike: a sine/triangle morph times an exponential decay envelope, with a cosine
	fade over the last 5%. Here we synthesize the same voice live at Rack's sample
	rate so it's sample-rate independent.

	Deviation from hardware: the Nano's pitch CV is inverted (higher V = lower
	pitch) due to wiring; this port uses standard non-inverted 1V/Oct so it tracks
	sequencers as expected.
*/

struct Claves : Module {
	enum ParamId {
		DECAY_PARAM,
		WAVE_PARAM,
		PITCH_PARAM,
		TRIG_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		TRIG_INPUT,   // IN1
		IN2_INPUT,    // unused by this firmware
		CV_INPUT,     // pitch (1V/Oct)
		INPUTS_LEN
	};
	enum OutputId {
		AUDIO_OUTPUT, // OUT
		OUTPUTS_LEN
	};
	enum LightId {
		ENV_LIGHT,
		LIGHTS_LEN
	};

	// Voice state lives in the shared core (same synthesis as the firmware).
	sc::ClavesVoice voice;

	dsp::SchmittTrigger gateTrigger;
	dsp::BooleanTrigger buttonTrigger;

	Claves() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(DECAY_PARAM, 0.f, 1.f, 0.3f, "Decay");
		configParam(WAVE_PARAM, 0.f, 1.f, 0.8f, "Waveform (sine <-> triangle)");
		configParam(PITCH_PARAM, 0.f, 1.f, 0.7f, "Pitch");
		configButton(TRIG_PARAM, "Manual trigger");

		configInput(TRIG_INPUT, "IN1 trigger");
		configInput(IN2_INPUT, "IN2 (unused)");
		configInput(CV_INPUT, "1V/Oct pitch");
		configOutput(AUDIO_OUTPUT, "Audio");
	}

	// Sample knobs/CV and start a new strike (firmware onTrigger()).
	void strike() {
		const float decayRate = 1.f + 9.f * params[DECAY_PARAM].getValue();
		const float waveMorph = params[WAVE_PARAM].getValue();

		// Pitch: knob sets 0..5 "volts", CV adds 1V/Oct on top. f = 50 * 2^level.
		const float level = params[PITCH_PARAM].getValue() * 5.f + inputs[CV_INPUT].getVoltage();
		const float freq = clamp(50.f * std::pow(2.f, level), 1.f, 1500.f);

		voice.strike(decayRate, waveMorph, freq);
	}

	void onReset() override {
		voice.reset();
	}

	void process(const ProcessArgs& args) override {
		// Fire on IN1 rising edge or a button press.
		const bool gate = gateTrigger.process(inputs[TRIG_INPUT].getVoltage(), 0.1f, 1.f);
		const bool button = buttonTrigger.process(params[TRIG_PARAM].getValue() > 0.5f);
		if (gate || button)
			strike();

		// One sample from the shared core: audio in -1..1, envelope in 0..1.
		const sc::ClavesFrame f = voice.process(args.sampleTime);
		outputs[AUDIO_OUTPUT].setVoltage(f.audio * 5.f); // -1..1 -> +/-5V (Vpp 10V)
		lights[ENV_LIGHT].setBrightnessSmooth(f.env, args.sampleTime);
	}
};

struct ClavesWidget : ModuleWidget {
	ClavesWidget(Claves* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Claves.svg")));

		// 4 HP panel (19.8 mm) converted from the mod2-claves KiCad faceplate;
		// positions are the real hole centres (panel-local mm, from scripts/panels/tools).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.03f, 21.70f)), module, Claves::DECAY_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 40.06f)), module, Claves::WAVE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 58.42f)), module, Claves::PITCH_PARAM));

		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, Claves::TRIG_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, Claves::ENV_LIGHT));

		// Jacks mapped to the printed labels: Trigger (top-left), IN2 unused
		// (top-right), Out audio (bottom-left), Pitch CV (bottom-right).
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, Claves::TRIG_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.30f)), module, Claves::IN2_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, Claves::AUDIO_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, Claves::CV_INPUT));
	}
};

Model* modelClaves = createModel<Claves, ClavesWidget>("Claves");
