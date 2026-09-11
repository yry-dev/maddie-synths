#include "plugin.hpp"
#include <ByteCore.h>  // Shared bytebeat engine (also used by the mod2-byte firmware)

/*
	Byte — bytebeat emitter, sixteen formulas.

	Port of firmwares/mod2-byte/mod2-byte.ino, itself a port of Sean Luke's
	GRAINS `byte` firmware (github.com/eclab/grains), used under the Apache
	License 2.0, Copyright 2024 Sean Luke. The upstream notice ships beside the
	firmware as firmwares/mod2-byte/LICENSE.md and Apache requires it to travel
	with every copy — unlike the CC0 HAGIWO modules in this plugin, which carry
	no such condition.

	A bytebeat is one C expression over a free-running 32-bit counter `t` whose
	low eight bits are the sample. The sixteen expressions, the rate divider and
	the level stage are sc::ByteVoice, shared verbatim with the firmware; this
	file owns only Rack I/O.

	Mirrors the Mod2 panel (3 pots, 1 button, 1 LED, 4 jacks):
		POT1 PITCH -> 16-step pitch scaling; step 8 is upstream's default, below
		              it the formula is evaluated less often (slower, lower),
		              above it `t` advances in bigger strides (faster, and a
		              different waveform — skipping values of t is not the same
		              as playing them slowly)
		POT2 LEVEL -> output level, 32 steps; 16 is unity and the top of the dial
		              overdrives into the clamp, as on the hardware
		POT3 BYTE  -> which of the sixteen formulas; changing it restarts `t`
		BUTTON     -> short press = cycle the base rate; long press = restart
		            (exactly the firmware's short/long button)
		LED        -> decaying peak of the output
		IN1        -> reset: a rising edge restarts `t` (upstream's DIGITAL OUT)
		IN2        -> the auxiliary variable `x`
		CV         -> added to the BYTE selector, so a ramp scans the formulas
		OUT        -> audio output

	Base rate, formula and the aux variable are also in the right-click menu.

	Divergences from the firmware, both in Rack's favour and both because Rack
	has no pin map to obey:
	  - IN2 is a continuous 0..10 V input for `x` (0..255), which is what
	    upstream's IN3 was. The firmware can only gate it, because the MOD2's
	    IN2 lands on GPIO0 and that pin has no ADC.
	  - CV sums with the POT3 knob rather than sharing an ADC with it. Same
	    control, minus the hardware's normalling.
	None of the stock sixteen formulas read `x`; it is there for anyone editing
	the table in ByteCore.h, exactly as upstream intended.

	License:
	Apache License 2.0, Copyright 2024 Sean Luke — the bytebeat engine this
	module drives is a port of his GRAINS `byte` firmware, and Apache requires
	that notice to ship with it; it lives at firmwares/mod2-byte/LICENSE.md
	together with the list of changes.

	The Rack-side work in this file is MIT License, Copyright (c) 2026 Madelyn
	Yeary, see rack-plugins/LICENSE.md.
*/

static const std::vector<std::string> kByteRateLabels = {
	"8000 Hz", "11025 Hz", "16384 Hz (GRAINS)", "22050 Hz"
};

struct Byte : Mod2Module {
	enum ParamId {
		PITCH_PARAM,
		LEVEL_PARAM,
		BYTE_PARAM,
		BTN_PARAM,
		RATE_PARAM,   // no panel widget — cycled by the button short-press + menu
		PARAMS_LEN
	};
	enum InputId {
		RESET_INPUT,  // IN1: rising edge restarts t
		AUX_INPUT,    // IN2: the auxiliary variable x, 0..10 V -> 0..255
		CV_INPUT,     // CV: added to the formula selector
		INPUTS_LEN
	};
	enum OutputId {
		AUDIO_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		LEVEL_LIGHT,
		LIGHTS_LEN
	};

	// The bytebeat itself lives in the shared core (same arithmetic as the
	// firmware, evaluated at its own base rate and held in between).
	sc::ByteVoice voice;
	dsp::SchmittTrigger resetTrigger;

	// Short/long press detection on the single panel button (firmware behavior).
	float btnHeld = 0.f;
	bool btnPrev = false;
	bool longFired = false;
	static constexpr float kLongPress = 0.5f;  // seconds, as in the firmware

	Byte() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(PITCH_PARAM, 0.f, 1.f, 0.5f, "Pitch scaling");
		configParam(LEVEL_PARAM, 0.f, 1.f, 0.5f, "Level");
		configParam(BYTE_PARAM, 0.f, 1.f, 0.f, "Bytebeat formula");
		configButton(BTN_PARAM, "Tap to cycle base rate, hold to restart");
		configSwitch(RATE_PARAM, 0.f, (float) (sc::kByteNumRates - 1),
			(float) sc::kByteDefaultRate, "Base rate", kByteRateLabels);

		configInput(RESET_INPUT, "Reset");
		configInput(AUX_INPUT, "Auxiliary variable x (0..10V)");
		configInput(CV_INPUT, "Formula select CV");
		configOutput(AUDIO_OUTPUT, "Audio");
	}

	void onReset() override {
		voice.reset();
	}

	void process(const ProcessArgs& args) override {
		// Single button: short press cycles the base rate, long press restarts.
		const bool btn = params[BTN_PARAM].getValue() > 0.5f;
		if (btn) {
			btnHeld += args.sampleTime;
			if (!longFired && btnHeld >= kLongPress) {
				voice.restart();
				longFired = true;
			}
		}
		else {
			if (btnPrev && !longFired) {
				int n = (int) std::round(params[RATE_PARAM].getValue());
				params[RATE_PARAM].setValue((float) ((n + 1) % sc::kByteNumRates));
			}
			btnHeld = 0.f;
			longFired = false;
		}
		btnPrev = btn;

		voice.setRateIndex((int) std::round(params[RATE_PARAM].getValue()));

		if (resetTrigger.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f))
			voice.restart();

		voice.setPitch(params[PITCH_PARAM].getValue());
		voice.setLevel(params[LEVEL_PARAM].getValue());
		// CV rides on top of the knob: 10 V sweeps the whole table, so a ramp or
		// a sequencer scans the sixteen formulas.
		voice.setExpression(params[BYTE_PARAM].getValue()
			+ inputs[CV_INPUT].getVoltage() / 10.f);
		voice.setAux(inputs[AUX_INPUT].getVoltage() / 10.f);

		const sc::ByteFrame f = voice.process(args.sampleTime);
		outputs[AUDIO_OUTPUT].setVoltage(f.audio * 5.f);  // -1..1 -> +/-5V
		lights[LEVEL_LIGHT].setBrightnessSmooth(f.env, args.sampleTime);
	}
};

struct ByteWidget : ModuleWidget {
	ByteWidget(Byte* module) {
		setModule(module);
		setMod2Panel(this, module, "res/mod2-byte.svg");

		// 4 HP Mod1/Mod2 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<Reversed<RoundBlackKnob>>(mm2px(Vec(10.03f, 21.7f)), module, Byte::PITCH_PARAM));
		addParam(createParamCentered<Reversed<RoundBlackKnob>>(mm2px(Vec(10.04f, 40.06f)), module, Byte::LEVEL_PARAM));
		addParam(createParamCentered<Reversed<RoundBlackKnob>>(mm2px(Vec(10.04f, 58.42f)), module, Byte::BYTE_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, Byte::BTN_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, Byte::LEVEL_LIGHT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, Byte::RESET_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.3f)), module, Byte::AUX_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, Byte::AUDIO_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, Byte::CV_INPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Byte* m = dynamic_cast<Byte*>(module);
		assert(m);
		menu->addChild(new MenuSeparator);
		menu->addChild(createIndexSubmenuItem("Base rate", kByteRateLabels,
			[=]() { return (int) std::round(m->params[Byte::RATE_PARAM].getValue()); },
			[=](int i) { m->params[Byte::RATE_PARAM].setValue((float) i); }));
		appendMod2PanelMenu(menu, module);
	}
};

Model* modelByte = createModel<Byte, ByteWidget>("mod2-byte");
