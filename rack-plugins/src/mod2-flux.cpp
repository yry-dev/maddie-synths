#include "plugin.hpp"
#include <FluxCore.h>  // Shared Flux voice (also used by mod2-flux firmware)

/*
	Flux — physical-modelling / resonance / noise multi-mode voice.

	Port of firmwares/mod2-flux/mod2-flux.ino (HAGIWO Mod2, RP2350).

	Seven modes in three groups:
		RESONANCE: 0 Modal (tuned resonator bank), 1 Karplus (plucked string)
		NOISE:     2 White, 3 Pink (1/f), 4 S&H (stepped), 5 Quantum (Lorenz)
		TEXTURE:   6 Drone (evolving harmonic texture)

	Mirrors the Mod2 panel (3 pots, 1 button, 1 LED, 4 jacks):
		POT1 FREQ   -> centre / pitch frequency
		POT2 RATE   -> auto-trigger speed / chaos rate
		POT3 CHAR   -> per-mode character / brightness / slew
		BUTTON      -> short press = cycle mode; long press = manual trigger / pluck
		             (exactly the firmware's short/long button)
		LED         -> blinks on every trigger (auto or manual)
		IN1         -> trigger / gate (manual excite, rising edge)
		IN2         -> unused (hardware: N/A)
		CV          -> 1V/Oct pitch
		OUT         -> audio output

	The mode is also selectable from the right-click menu. The Modal and Karplus
	modes self-trigger on the RATE control, and Drone slowly re-randomises its
	harmonic balance — all inside the shared core, so this port runs the same way
	the hardware does.

	Deviation from hardware: on the Mod2 the CV jack is wired to the character pot;
	this port routes CV to 1V/Oct pitch, since a pitched voice is far more useful
	in a rack (CHARACTER stays on its own knob).
*/

struct Flux : Module {
	enum ParamId {
		FREQ_PARAM,
		RATE_PARAM,
		CHAR_PARAM,
		TRIG_PARAM,
		MODE_PARAM,  // no panel widget — cycled by the button short-press + menu
		PARAMS_LEN
	};
	enum InputId {
		TRIG_INPUT,   // IN1
		IN2_INPUT,    // unused by this firmware
		CV_INPUT,     // 1V/Oct pitch
		INPUTS_LEN
	};
	enum OutputId {
		AUDIO_OUTPUT, // OUT
		OUTPUTS_LEN
	};
	enum LightId {
		TRIG_LIGHT,
		LIGHTS_LEN
	};

	static constexpr int kNumModes = 7;

	// Voice state lives in the shared core (same synthesis as the firmware).
	sc::FluxVoice voice;

	dsp::SchmittTrigger gateTrigger;
	dsp::PulseGenerator ledPulse;

	// Short/long press detection on the single panel button (firmware behavior).
	float btnHeld = 0.f;
	bool btnPrev = false;
	bool longFired = false;
	static constexpr float kLongPress = 0.4f; // seconds

	Flux() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(FREQ_PARAM, 0.f, 1.f, 0.5f, "Frequency");
		configParam(RATE_PARAM, 0.f, 1.f, 0.5f, "Rate / chaos rate");
		configParam(CHAR_PARAM, 0.f, 1.f, 0.5f, "Character / brightness / slew");
		configButton(TRIG_PARAM, "Tap to cycle mode, hold to pluck");
		configSwitch(MODE_PARAM, 0.f, 6.f, 0.f, "Mode",
			{"Modal", "Karplus", "White", "Pink", "S&H", "Quantum", "Drone"});

		configInput(TRIG_INPUT, "IN1 trigger/gate");
		configInput(IN2_INPUT, "IN2 (unused)");
		configInput(CV_INPUT, "1V/Oct pitch");
		configOutput(AUDIO_OUTPUT, "Audio");

		voice.reset();
	}

	void onReset() override {
		voice.reset();
	}

	void process(const ProcessArgs& args) override {
		voice.setMode((uint8_t) std::round(params[MODE_PARAM].getValue()));

		// Pitch: knob sets 0..5 "volts", CV adds 1V/Oct on top. f = 32 * 2^level.
		const float level = params[FREQ_PARAM].getValue() * 5.f + inputs[CV_INPUT].getVoltage();
		const float freq = clamp(32.f * std::pow(2.f, level), 32.f, 2000.f);
		voice.setParams(freq, params[RATE_PARAM].getValue(), params[CHAR_PARAM].getValue());

		// Single button: short press cycles mode, long press triggers/plucks.
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
			if (btnPrev && !longFired) {
				// short release → advance mode
				int n = (int) std::round(params[MODE_PARAM].getValue());
				params[MODE_PARAM].setValue((float) ((n + 1) % kNumModes));
			}
			btnHeld = 0.f;
			longFired = false;
		}
		btnPrev = btn;

		// Fire on IN1 rising edge or a long button press.
		const bool gate = gateTrigger.process(inputs[TRIG_INPUT].getVoltage(), 0.1f, 1.f);
		if (gate || buttonTrig)
			voice.trigger();

		const float audio = voice.process(args.sampleTime);  // -1..+1
		if (voice.consumeFired())
			ledPulse.trigger(0.05f);

		outputs[AUDIO_OUTPUT].setVoltage(audio * 5.f);  // -1..1 -> +/-5V (Vpp 10V)
		const float led = ledPulse.process(args.sampleTime) ? 1.f : 0.f;
		lights[TRIG_LIGHT].setBrightnessSmooth(led, args.sampleTime);
	}
};

struct FluxWidget : ModuleWidget {
	FluxWidget(Flux* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/mod2-flux.svg")));

		// 4 HP panel (19.8 mm): hole centres from the mod2-flux KiCad faceplate
		// (panel-local mm, scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.03f, 21.70f)), module, Flux::FREQ_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 40.06f)), module, Flux::RATE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 58.42f)), module, Flux::CHAR_PARAM));

		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, Flux::TRIG_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, Flux::TRIG_LIGHT));

		// Jacks: IN1 trigger (top-left), IN2 unused (top-right), OUT (bottom-left), CV (bottom-right).
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, Flux::TRIG_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.30f)), module, Flux::IN2_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, Flux::AUDIO_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, Flux::CV_INPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Flux* m = dynamic_cast<Flux*>(module);
		assert(m);
		menu->addChild(new MenuSeparator);
		menu->addChild(createIndexSubmenuItem("Mode",
			{"Modal", "Karplus", "White", "Pink", "S&H", "Quantum", "Drone"},
			[=]() { return (int) std::round(m->params[Flux::MODE_PARAM].getValue()); },
			[=](int i) { m->params[Flux::MODE_PARAM].setValue((float) i); }));
	}
};

Model* modelFlux = createModel<Flux, FluxWidget>("mod2-flux");
