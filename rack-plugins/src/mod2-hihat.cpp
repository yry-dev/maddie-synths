#include "plugin.hpp"
#include <HihatCore.h>  // Shared Hi-hat voice (also used by mod2-hihat firmware)

/*
	Hihat — white/blue-noise hi-hat percussion voice.

	Port of firmwares/mod2-hihat/mod2-hihat.ino (HAGIWO Mod2, RP2350).

	Band-pass filtered noise shaped by an exponential decay, rendered live at
	Rack's sample rate by the shared sc::HihatVoice (the firmware bakes the same
	core into a table per strike). Mirrors the Mod2 panel (3 pots, 1 button, 1
	LED, 4 jacks):
		POT1   -> Decay time
		POT2   -> Decay curve
		POT3   -> BPF frequency (shares the CV jack on hardware)
		BUTTON -> short press = manual trigger; long press = toggle noise type
		         (blue / white) — exactly the firmware's short/long button.
		LED    -> envelope brightness
		IN1    -> trigger input (rising edge)
		IN2    -> accent (lowers volume by 6 dB while high)
		CV     -> BPF frequency
		OUT    -> audio output

	The noise type is also available in the right-click menu. Deviation from
	hardware: the firmware's pot/CV slope is inverted (A2 wiring); this port uses
	the natural non-inverted direction (CW / higher CV = brighter).
*/

struct Hihat : Module {
	enum ParamId {
		DECAY_PARAM,
		CURVE_PARAM,
		FREQ_PARAM,
		TRIG_PARAM,
		NOISE_PARAM,  // no panel widget — driven by the button long-press + menu
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

	// Short/long press detection on the single panel button (firmware behavior).
	float btnHeld = 0.f;
	bool btnPrev = false;
	bool longFired = false;
	static constexpr float kLongPress = 0.4f; // seconds

	Hihat() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(DECAY_PARAM, 0.f, 1.f, 0.5f, "Decay time");
		configParam(CURVE_PARAM, 0.f, 1.f, 0.5f, "Decay curve");
		configParam(FREQ_PARAM, 0.f, 1.f, 0.5f, "BPF frequency");
		configButton(TRIG_PARAM, "Trigger (hold to switch noise type)");
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

		// Single button: short press triggers, long press toggles noise type.
		const bool btn = params[TRIG_PARAM].getValue() > 0.5f;
		bool buttonStrike = false;
		if (btn) {
			btnHeld += args.sampleTime;
			if (!longFired && btnHeld >= kLongPress) {
				params[NOISE_PARAM].setValue(params[NOISE_PARAM].getValue() > 0.5f ? 0.f : 1.f);
				longFired = true;
			}
		}
		else {
			if (btnPrev && !longFired) buttonStrike = true; // released before long
			btnHeld = 0.f;
			longFired = false;
		}
		btnPrev = btn;

		// Fire on IN1 rising edge or a short button press.
		const bool gate = gateTrigger.process(inputs[TRIG_INPUT].getVoltage(), 0.1f, 1.f);
		if (gate || buttonStrike)
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
		setPanel(createPanel(asset::plugin(pluginInstance, "res/mod2-hihat.svg")));

		// 4 HP panel (19.8 mm): hole centres from the mod2-hihat KiCad faceplate
		// (panel-local mm, scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.03f, 21.70f)), module, Hihat::DECAY_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 40.06f)), module, Hihat::CURVE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 58.42f)), module, Hihat::FREQ_PARAM));

		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, Hihat::TRIG_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, Hihat::ENV_LIGHT));

		// Jacks: IN1 trigger (top-left), IN2 accent (top-right), OUT (bottom-left), CV (bottom-right).
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, Hihat::TRIG_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.30f)), module, Hihat::ACCENT_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, Hihat::AUDIO_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, Hihat::CV_INPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Hihat* m = dynamic_cast<Hihat*>(module);
		assert(m);
		menu->addChild(new MenuSeparator);
		menu->addChild(createIndexSubmenuItem("Noise type", {"Blue", "White"},
			[=]() { return (int) m->params[Hihat::NOISE_PARAM].getValue(); },
			[=](int i) { m->params[Hihat::NOISE_PARAM].setValue((float) i); }));
	}
};

Model* modelHihat = createModel<Hihat, HihatWidget>("mod2-hihat");
