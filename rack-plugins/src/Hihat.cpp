#include "plugin.hpp"
#include <HihatCore.h>  // Shared Hi-hat voice (also used by mod2-hihat firmware)

/*
	Hihat — white/blue-noise hi-hat percussion voice.

	Port of firmwares/mod2-hihat/mod2-hihat.ino (HAGIWO Mod2, RP2350).

	Band-pass filtered noise shaped by an exponential decay, rendered live at
	Rack's sample rate by the shared sc::HihatVoice (the firmware bakes the same
	core into a table per strike). Mirrors the Mod2 panel:
		POT1   -> Decay time
		POT2   -> Decay curve
		POT3   -> BPF frequency (shares the CV jack on hardware)
		BUTTON -> manual trigger
		SWITCH -> noise type (blue / white) — the firmware's long-press toggle
		IN1    -> trigger input (rising edge)
		IN2    -> accent (lowers volume by 6 dB while high)
		CV     -> BPF frequency
		OUT    -> audio output

	Deviation from hardware: the firmware's pot/CV slope is inverted (A2 wiring);
	this port uses the natural non-inverted direction (CW / higher CV = brighter).
*/

struct Hihat : Module {
	enum ParamId {
		DECAY_PARAM,
		CURVE_PARAM,
		FREQ_PARAM,
		TRIG_PARAM,
		NOISE_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		TRIG_INPUT,   // IN1
		ACCENT_INPUT, // IN2
		CV_INPUT,     // BPF frequency
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

	sc::HihatVoice voice;
	float accentGain = 1.f; // captured at trigger time (1.0 or 0.5)

	dsp::SchmittTrigger gateTrigger;
	dsp::SchmittTrigger accentTrigger;
	dsp::BooleanTrigger buttonTrigger;

	Hihat() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(DECAY_PARAM, 0.f, 1.f, 0.5f, "Decay time");
		configParam(CURVE_PARAM, 0.f, 1.f, 0.5f, "Decay curve");
		configParam(FREQ_PARAM, 0.f, 1.f, 0.5f, "BPF frequency");
		configButton(TRIG_PARAM, "Manual trigger");
		configSwitch(NOISE_PARAM, 0.f, 1.f, 0.f, "Noise type", {"Blue", "White"});

		configInput(TRIG_INPUT, "IN1 trigger");
		configInput(ACCENT_INPUT, "IN2 accent");
		configInput(CV_INPUT, "BPF frequency CV");
		configOutput(AUDIO_OUTPUT, "Audio");
	}

	// Sample knobs/CV and start a new strike (firmware buildVoice() on trigger).
	void strike(float sampleRate) {
		const float decayBase = 0.1f + 9.0f * params[DECAY_PARAM].getValue();
		const float decayCurve = 0.2f + 5.0f * params[CURVE_PARAM].getValue();

		// Frequency: knob 100..16000 Hz, CV (0..5V) adds on top, non-inverted.
		const float freqNorm = clamp(params[FREQ_PARAM].getValue() + inputs[CV_INPUT].getVoltage() / 5.f, 0.f, 1.f);
		const float fc = 100.f + 15900.f * freqNorm;

		const sc::HihatNoiseMode mode = params[NOISE_PARAM].getValue() > 0.5f ? sc::kHihatWhite : sc::kHihatBlue;

		// Accent: IN2 high lowers volume by 6 dB (captured at the strike).
		accentGain = (accentTrigger.isHigh() || inputs[ACCENT_INPUT].getVoltage() > 1.f) ? 0.5f : 1.f;

		voice.strike(decayBase, decayCurve, fc, mode, sampleRate);
	}

	void onReset() override {
		voice.reset();
		accentGain = 1.f;
	}

	void process(const ProcessArgs& args) override {
		// Track the accent gate level so strike() can read it.
		accentTrigger.process(inputs[ACCENT_INPUT].getVoltage(), 0.1f, 1.f);

		// Fire on IN1 rising edge or a button press.
		const bool gate = gateTrigger.process(inputs[TRIG_INPUT].getVoltage(), 0.1f, 1.f);
		const bool button = buttonTrigger.process(params[TRIG_PARAM].getValue() > 0.5f);
		if (gate || button)
			strike(args.sampleRate);

		// One sample from the shared core: audio in -1..1, envelope in 0..1.
		const sc::HihatFrame f = voice.process(args.sampleTime);
		outputs[AUDIO_OUTPUT].setVoltage(f.audio * accentGain * 5.f); // -1..1 -> +/-5V
		lights[ENV_LIGHT].setBrightnessSmooth(f.env, args.sampleTime);
	}
};

struct HihatWidget : ModuleWidget {
	HihatWidget(Hihat* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Hihat.svg")));

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		const float cx = 15.24f; // 6 HP center

		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(cx, 18.5f)), module, Hihat::ENV_LIGHT));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(cx, 30.f)), module, Hihat::DECAY_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(cx, 47.f)), module, Hihat::CURVE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(cx, 64.f)), module, Hihat::FREQ_PARAM));

		// Noise-type switch (left) and manual trigger button (right).
		addParam(createParamCentered<CKSS>(mm2px(Vec(8.f, 82.f)), module, Hihat::NOISE_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(20.32f, 82.f)), module, Hihat::TRIG_PARAM));

		// Jacks: row 1 IN1 + CV, row 2 IN2 + OUT.
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.16f, 98.f)), module, Hihat::TRIG_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(20.32f, 98.f)), module, Hihat::CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.16f, 113.f)), module, Hihat::ACCENT_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(20.32f, 113.f)), module, Hihat::AUDIO_OUTPUT));
	}
};

Model* modelHihat = createModel<Hihat, HihatWidget>("Hihat");
