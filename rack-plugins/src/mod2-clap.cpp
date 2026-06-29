#include "plugin.hpp"
#include <ClapCore.h>  // Shared clap voice (also used by mod2-clap firmware)

/*
	Clap — TR-808-style hand-clap percussion voice.

	Port of firmwares/mod2-clap/mod2-clap.ino (HAGIWO Mod2, RP2350).

	Mirrors the Mod2 hardware: 3 pots, 1 push button, 1 PWM LED, and the Mod2
	jack set (IN1, IN2, CV, OUT). For the Clap firmware:
		POT1 (A0) -> Q    (BPF bandwidth 0.5–4.0; left=wide, right=narrow)
		POT2 (A1) -> Decay (tail time 20–200 ms)
		POT3 (A2) -> Freq  (BPF centre 50 Hz–8 kHz; shares the CV jack)
		BUTTON    -> manual trigger
		LED       -> envelope brightness
		IN1       -> trigger input (rising edge)
		IN2       -> accent (HIGH = −6 dB, 0.5× level)
		CV        -> BPF centre frequency (0–5 V modulates over full range)
		OUT       -> audio output

	The firmware pre-renders a 22 k-sample table on each trigger (burst+tail
	envelopes, band-pass filtered noise). Here the same voice is synthesized live
	at Rack's sample rate via the shared ClapCore so it is sample-rate
	independent.

	Deviation from hardware: the firmware's POT3/CV share pin A2 so they are
	mutually exclusive; here the knob sets the base frequency and the CV jack
	modulates on top (0 V = no offset, 5 V = full range), matching the VCV
	convention of summing knob and CV.
*/

struct Clap : Module {
	enum ParamId {
		DECAY_PARAM,
		TONE_PARAM,
		FREQ_PARAM,
		TRIG_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		TRIG_INPUT,    // IN1 — trigger (rising edge)
		IN2_INPUT,     // IN2 — accent (>1 V = −6 dB)
		CV_INPUT,      // CV  — BPF centre freq modulation (0–5 V)
		INPUTS_LEN
	};
	enum OutputId {
		AUDIO_OUTPUT,  // OUT
		OUTPUTS_LEN
	};
	enum LightId {
		ENV_LIGHT,
		LIGHTS_LEN
	};

	// Shared synthesis core (same algorithm as the firmware).
	sc::ClapCore core;

	dsp::SchmittTrigger gateTrigger;
	dsp::BooleanTrigger buttonTrigger;

	// Cached at process() time so strike() can pass the correct fs to the core.
	float sampleRate = 44100.f;
	// Volume scale captured at trigger time (accent via IN2).
	float vol = 1.f;

	Clap() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(DECAY_PARAM, 0.f, 1.f, 0.5f, "Decay", " ms", 0.f, 180.f, 20.f);
		configParam(TONE_PARAM,  0.f, 1.f, 0.5f, "BPF Q (tone)");
		configParam(FREQ_PARAM,  0.f, 1.f, 0.18f, "BPF frequency", " Hz", 0.f, 7950.f, 50.f);
		configButton(TRIG_PARAM, "Manual trigger");

		configInput(TRIG_INPUT, "IN1 trigger");
		configInput(IN2_INPUT,  "IN2 accent (>1 V = −6 dB)");
		configInput(CV_INPUT,   "BPF centre frequency CV (0–5 V)");
		configOutput(AUDIO_OUTPUT, "Audio");
	}

	// Sample knobs/CV and fire a new strike (mirrors firmware onTrigger logic).
	void strike() {
		// POT2 A1: decay 20–200 ms
		const float decayMs = 20.f + 180.f * params[DECAY_PARAM].getValue();
		// POT1 A0: Q 0.5–4.0
		const float q = 0.5f + 3.5f * params[TONE_PARAM].getValue();
		// POT3 A2 (+ CV): BPF centre 50–8000 Hz.
		// Knob is non-inverted (CCW=50 Hz, CW=8 kHz) — more intuitive than the
		// hardware's inverted wiring.  CV (0–5 V) modulates additively over the
		// same full range.
		const float freqKnob = params[FREQ_PARAM].getValue();
		const float freqCV   = inputs[CV_INPUT].getNormalVoltage(0.f) / 5.f;
		const float fc = clamp(50.f + 7950.f * (freqKnob + freqCV), 50.f, 8000.f);

		// IN2 accent: >1 V → −6 dB (0.5×)
		vol = inputs[IN2_INPUT].getVoltage() > 1.f ? 0.5f : 1.f;

		core.strike(decayMs, fc, q, sampleRate);
	}

	void onReset() override {
		core.reset();
		vol = 1.f;
	}

	void process(const ProcessArgs& args) override {
		sampleRate = args.sampleRate;

		// Fire on IN1 rising edge or a button press.
		const bool gate   = gateTrigger.process(inputs[TRIG_INPUT].getVoltage(), 0.1f, 1.f);
		const bool button = buttonTrigger.process(params[TRIG_PARAM].getValue() > 0.5f);
		if (gate || button)
			strike();

		// One sample from the shared core: audio in −1..+1, envelope in 0..1.
		const sc::ClapFrame f = core.process(args.sampleTime);
		outputs[AUDIO_OUTPUT].setVoltage(f.audio * vol * 5.f);
		lights[ENV_LIGHT].setBrightnessSmooth(f.env, args.sampleTime);
	}
};

struct ClapWidget : ModuleWidget {
	ClapWidget(Clap* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/mod2-clap.svg")));
		// 4 HP Mod1/Mod2 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.03f, 21.7f)), module, Clap::DECAY_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 40.06f)), module, Clap::TONE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 58.42f)), module, Clap::FREQ_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, Clap::TRIG_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, Clap::ENV_LIGHT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, Clap::TRIG_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.3f)), module, Clap::IN2_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, Clap::AUDIO_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, Clap::CV_INPUT));
	}
};

Model* modelClap = createModel<Clap, ClapWidget>("mod2-clap");
