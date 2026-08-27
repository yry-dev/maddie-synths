#include "plugin.hpp"
#include <CrackleCore.h>  // Shared Crackle voice (also used by the firmware)

/*
	Crackle — random snaps, crackles and pops: worn vinyl, or an old radio
	hunting between stations.

	Port of firmwares/mod2-crackle/mod2-crackle.ino, itself a port of Sean Luke's
	GRAINS `crackle`. The whole model — when crackles arrive, how long each one
	lasts, how loud it is — is sc::CrackleVoice, shared verbatim with the
	firmware; this file owns only the Rack I/O.

	Mirrors the Mod2 panel (3 pots, 1 button, 1 LED, 4 jacks):
		POT1 DENSITY -> crackles per second, ~1 Hz (CCW) to ~128 Hz (CW)
		POT2 GAIN    -> crackle volume AND volume variance (see below)
		POT3 LENGTH  -> longest crackle a draw can produce, 0..15.6 ms
		BUTTON       -> short press swaps analog/digital output; long press pops
		LED          -> peak-hold flash on every crackle
		IN1          -> trigger: one crackle per rising edge
		IN2          -> accent gate: while high, every crackle is full volume
		CV           -> DENSITY (see the divergence note below)
		OUT          -> audio

	GAIN is a variance control, which is the part worth knowing: each burst
	sample is a uniform value scaled by a second uniform value and then clipped,
	so at low settings crackles are quiet and all different from one another, and
	as you turn up more and more of them slam into the clip and arrive at
	identical maximum volume. That is upstream's behaviour and the reason the
	knob is worth sweeping rather than setting.

	Two output modes, because GRAINS had two output jacks and Mod2 has one:
	ANALOG is the noisy, GAIN-dependent crackle; DIGITAL is upstream's gate
	output — unipolar 0..+10 V pulses, always full amplitude, GAIN ignored.
	Digital is the one to patch into a CV input. The mode is on the button (and
	the right-click menu) and saves with the patch.

	Divergences from the firmware:
	  - CV goes to DENSITY. On the Mod2 the CV jack shares POT3's ADC pin, so the
	    firmware can only sum CV into LENGTH; Rack has a free jack, so CV is
	    routed to DENSITY the way GRAINS wired it (its IN1 was the rate CV).
	    +/-5 V spans the full density range on top of the knob.
	  - The button's long-press pop is also just a mouse-down-and-hold here, so
	    TRIG_PARAM doubles as the manual trigger; short click still swaps mode.

	The firmware header lists the divergences from GRAINS itself (inverted
	DENSITY, instant pot response, sign-randomised bursts, gate-instead-of-CV on
	IN2); all of them live in the shared core, so this port inherits them.

	License:
	Apache License 2.0, Copyright 2024 Sean Luke (sean@cs.gmu.edu) — this module
	and its sc::CrackleVoice are derived from the GRAINS `crackle` firmware
	(github.com/eclab/grains), and Apache 2.0 permits modification and
	redistribution only so long as the license notice ships with every copy or
	substantial portion of the work, and modified files carry prominent notice of
	change. That notice is kept at firmwares/mod2-crackle/LICENSE.md; keep it
	there and ship it. Unlike the CC0 HAGIWO modules in this plugin, this one
	carries conditions.
	The Rack-side work in this file is MIT License, Copyright (c) 2026 Madelyn
	Yeary, see rack-plugins/LICENSE.md.
*/

struct Crackle : Mod2Module {
	enum ParamId {
		DENSITY_PARAM,
		GAIN_PARAM,
		LENGTH_PARAM,
		TRIG_PARAM,
		MODE_PARAM,  // no panel widget — swapped by the button + right-click menu
		PARAMS_LEN
	};
	enum InputId {
		TRIG_INPUT,    // IN1
		ACCENT_INPUT,  // IN2
		CV_INPUT,      // density CV
		INPUTS_LEN
	};
	enum OutputId { AUDIO_OUTPUT, OUTPUTS_LEN };
	enum LightId { CRACKLE_LIGHT, LIGHTS_LEN };

	// The whole model lives in the shared core (same code as the firmware).
	sc::CrackleVoice voice;

	dsp::SchmittTrigger gateTrigger;

	// Short/long press on the single panel button, exactly as the firmware does
	// it: hold past kLongPress fires a pop, a shorter click swaps output mode.
	float btnHeld = 0.f;
	bool btnPrev = false;
	bool longFired = false;
	static constexpr float kLongPress = 0.4f;  // seconds

	Crackle() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(DENSITY_PARAM, 0.f, 1.f, 0.5f, "Density");
		configParam(GAIN_PARAM, 0.f, 1.f, 0.7f, "Gain / volume variance");
		configParam(LENGTH_PARAM, 0.f, 1.f, 0.35f, "Crackle length");
		configButton(TRIG_PARAM, "Click to swap analog/digital, hold to pop");
		configSwitch(MODE_PARAM, 0.f, 1.f, 0.f, "Output", {"Analog", "Digital"});

		configInput(TRIG_INPUT, "IN1 trigger (one crackle per edge)");
		configInput(ACCENT_INPUT, "IN2 accent gate (full-volume crackles)");
		configInput(CV_INPUT, "Density CV");
		configOutput(AUDIO_OUTPUT, "Audio");

		voice.reset();
	}

	void onReset() override { voice.reset(); }

	void process(const ProcessArgs& args) override {
		const bool digital = params[MODE_PARAM].getValue() > 0.5f;
		voice.setDigital(digital);

		// +/-5 V of CV sweeps the whole density range on top of the knob, which
		// is the closest Rack equivalent of GRAINS summing its rate CV into POT1.
		const float density = clamp(params[DENSITY_PARAM].getValue() +
		                                inputs[CV_INPUT].getVoltage() * 0.2f,
		                            0.f, 1.f);
		voice.setParams(density, params[GAIN_PARAM].getValue(),
		                params[LENGTH_PARAM].getValue());

		// IN2 is a level gate on the hardware, so treat it as one here too
		// (>1 V, the accent threshold the other Mod2 drum ports use).
		voice.setAccent(inputs[ACCENT_INPUT].getVoltage() > 1.f);

		// Button: hold = manual pop, click = swap output mode.
		const bool btn = params[TRIG_PARAM].getValue() > 0.5f;
		bool buttonTrig = false;
		if (btn) {
			btnHeld += args.sampleTime;
			if (!longFired && btnHeld >= kLongPress) {
				buttonTrig = true;
				longFired = true;
			}
		}
		else {
			if (btnPrev && !longFired)
				params[MODE_PARAM].setValue(digital ? 0.f : 1.f);
			btnHeld = 0.f;
			longFired = false;
		}
		btnPrev = btn;

		if (gateTrigger.process(inputs[TRIG_INPUT].getVoltage(), 0.1f, 1.f) || buttonTrig)
			voice.trigger();

		const sc::CrackleFrame f = voice.process(args.sampleTime);
		// Analog crackles are bipolar audio; digital pops keep upstream's
		// unipolar gate shape, so they read as 0..+10 V rather than +/-5 V.
		outputs[AUDIO_OUTPUT].setVoltage(digital ? f.audio * 10.f : f.audio * 5.f);
		// Crackles last a few milliseconds — too short to see — so the light
		// gets the firmware's peak-hold treatment via the smoothing lambda.
		lights[CRACKLE_LIGHT].setBrightnessSmooth(f.env, args.sampleTime, 25.f);
	}
};

struct CrackleWidget : ModuleWidget {
	CrackleWidget(Crackle* module) {
		setModule(module);
		setMod2Panel(this, module, "res/mod2-crackle.svg");

		// 4 HP Mod1/Mod2 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<Reversed<RoundBlackKnob>>(mm2px(Vec(10.03f, 21.7f)), module, Crackle::DENSITY_PARAM));
		addParam(createParamCentered<Reversed<RoundBlackKnob>>(mm2px(Vec(10.04f, 40.06f)), module, Crackle::GAIN_PARAM));
		addParam(createParamCentered<Reversed<RoundBlackKnob>>(mm2px(Vec(10.04f, 58.42f)), module, Crackle::LENGTH_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, Crackle::TRIG_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, Crackle::CRACKLE_LIGHT));

		// The faceplate prints Out bottom-left and CV bottom-right, the same way
		// round as every other Mod2 panel; the sketch header's ASCII diagram has
		// that pair drawn the other way and the board is the one to believe.
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, Crackle::TRIG_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.3f)), module, Crackle::ACCENT_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, Crackle::AUDIO_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, Crackle::CV_INPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Crackle* m = dynamic_cast<Crackle*>(module);
		assert(m);
		menu->addChild(new MenuSeparator);
		menu->addChild(createIndexSubmenuItem("Output",
			{"Analog crackles", "Digital pops"},
			[=]() { return m->params[Crackle::MODE_PARAM].getValue() > 0.5f ? 1 : 0; },
			[=](int i) { m->params[Crackle::MODE_PARAM].setValue((float) i); }));
		appendMod2PanelMenu(menu, module);
	}
};

Model* modelCrackle = createModel<Crackle, CrackleWidget>("mod2-crackle");
