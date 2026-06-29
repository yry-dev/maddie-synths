#include "plugin.hpp"
#include <SpiralCore.h>  // Shared Spiral synthesis (also used by mod2-spiral firmware)

/*
	Spiral 4Ever — auditory-illusion / Shepard-tone multi-mode generator.

	Port of firmwares/mod2-spiral/mod2-spiral.ino (HAGIWO Mod2, RP2350).

	Nine modes:
		0 Shepard rising   1 Shepard falling   2 Barber pole
		3 Risset rhythm    4 Tritone paradox   5 Tritone explorer
		6 Shepard cluster maj   7 Shepard cluster min   8 Euler spiral

	Mirrors the Mod2 panel (3 pots, 1 button, 1 LED, 4 jacks):
		POT1 FREQ  -> window centre frequency, exponential (0..5 "octaves")
		POT2 SPEED -> sweep speed
		POT3 WIDTH -> envelope width / pitch class / spiral spread
		BUTTON     -> short press = cycle mode; long press = toggle direction
		            (exactly the firmware's short/long button)
		LED        -> lit when the sweep direction is "up"
		IN1, IN2   -> unused (hardware: N/A)
		CV         -> 1V/Oct added to the FREQ control
		OUT        -> audio output

	Mode and direction are also available in the right-click menu.

	Deviation from hardware: on the Mod2 the single CV jack is wired to A2 (shared
	with POT3). This port routes CV to the window centre as 1V/Oct so the illusion
	can track a sequencer (the same musical choice the Claves port made). The
	synthesis itself is the shared core, identical to the firmware.
*/

struct Spiral : Module {
	enum ParamId {
		FREQ_PARAM,
		SPEED_PARAM,
		WIDTH_PARAM,
		BTN_PARAM,
		MODE_PARAM,   // no panel widget — cycled by the button short-press + menu
		DIR_PARAM,    // no panel widget — toggled by the button long-press + menu
		PARAMS_LEN
	};
	enum InputId {
		IN1_INPUT,    // unused (hardware: N/A)
		IN2_INPUT,    // unused (hardware: N/A)
		CV_INPUT,     // 1V/Oct -> window centre
		INPUTS_LEN
	};
	enum OutputId {
		AUDIO_OUTPUT, // OUT
		OUTPUTS_LEN
	};
	enum LightId {
		DIR_LIGHT,    // lit when sweeping "up"
		LIGHTS_LEN
	};

	static constexpr int kNumModes = 9;

	// Synthesis state lives in the shared core (same DSP as the firmware).
	sc::SpiralCore voice;

	// Short/long press detection on the single panel button (firmware behavior).
	float btnHeld = 0.f;
	bool btnPrev = false;
	bool longFired = false;
	static constexpr float kLongPress = 0.4f; // seconds

	Spiral() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(FREQ_PARAM, 0.f, 1.f, 0.5f, "Center frequency");
		configParam(SPEED_PARAM, 0.f, 1.f, 0.4f, "Sweep speed");
		configParam(WIDTH_PARAM, 0.f, 1.f, 0.4f, "Width / pitch class / spread");
		configButton(BTN_PARAM, "Tap to cycle mode, hold to flip direction");
		configSwitch(MODE_PARAM, 0.f, 8.f, 0.f, "Mode", {
			"Shepard rising", "Shepard falling", "Barber pole",
			"Risset rhythm", "Tritone paradox", "Tritone explorer",
			"Shepard cluster maj", "Shepard cluster min", "Euler spiral"
		});
		configSwitch(DIR_PARAM, 0.f, 1.f, 1.f, "Direction", {"Down", "Up"});

		configInput(IN1_INPUT, "IN1 (unused)");
		configInput(IN2_INPUT, "IN2 (unused)");
		configInput(CV_INPUT, "1V/Oct (center frequency)");
		configOutput(AUDIO_OUTPUT, "Audio");
	}

	void onReset() override {
		voice.reset();
	}

	void process(const ProcessArgs& args) override {
		// Single button: short press cycles mode, long press flips direction.
		const bool btn = params[BTN_PARAM].getValue() > 0.5f;
		if (btn) {
			btnHeld += args.sampleTime;
			if (!longFired && btnHeld >= kLongPress) {
				params[DIR_PARAM].setValue(params[DIR_PARAM].getValue() > 0.5f ? 0.f : 1.f);
				longFired = true;
			}
		}
		else {
			if (btnPrev && !longFired) {
				int n = (int) std::round(params[MODE_PARAM].getValue());
				params[MODE_PARAM].setValue((float) ((n + 1) % kNumModes));
			}
			btnHeld = 0.f;
			longFired = false;
		}
		btnPrev = btn;

		voice.setMode((int) std::round(params[MODE_PARAM].getValue()));
		voice.setDirection(params[DIR_PARAM].getValue() > 0.5f);

		// FREQ knob spans 0..5 "octaves"; CV adds 1V/Oct on top.
		const float centerOct = params[FREQ_PARAM].getValue() * 5.f + inputs[CV_INPUT].getVoltage();
		voice.setParams(centerOct, params[SPEED_PARAM].getValue(), params[WIDTH_PARAM].getValue());

		const float sample = voice.process(args.sampleTime);
		outputs[AUDIO_OUTPUT].setVoltage(sample * 5.f); // -1..1 -> +/-5V (Vpp 10V)

		lights[DIR_LIGHT].setBrightness(voice.directionUp ? 1.f : 0.f);
	}
};

struct SpiralWidget : ModuleWidget {
	SpiralWidget(Spiral* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/mod2-spiral.svg")));

		// 4 HP panel (19.8 mm): hole centres from the mod2-spiral KiCad faceplate
		// (panel-local mm, scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.03f, 21.70f)), module, Spiral::FREQ_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 40.06f)), module, Spiral::SPEED_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 58.42f)), module, Spiral::WIDTH_PARAM));

		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, Spiral::BTN_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, Spiral::DIR_LIGHT));

		// Jacks: IN1 (top-left), IN2 (top-right), OUT (bottom-left), CV (bottom-right).
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, Spiral::IN1_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.30f)), module, Spiral::IN2_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, Spiral::AUDIO_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, Spiral::CV_INPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Spiral* m = dynamic_cast<Spiral*>(module);
		assert(m);
		menu->addChild(new MenuSeparator);
		menu->addChild(createIndexSubmenuItem("Mode", {
				"Shepard rising", "Shepard falling", "Barber pole",
				"Risset rhythm", "Tritone paradox", "Tritone explorer",
				"Shepard cluster maj", "Shepard cluster min", "Euler spiral"
			},
			[=]() { return (int) std::round(m->params[Spiral::MODE_PARAM].getValue()); },
			[=](int i) { m->params[Spiral::MODE_PARAM].setValue((float) i); }));
		menu->addChild(createIndexSubmenuItem("Direction", {"Down", "Up"},
			[=]() { return (int) std::round(m->params[Spiral::DIR_PARAM].getValue()); },
			[=](int i) { m->params[Spiral::DIR_PARAM].setValue((float) i); }));
	}
};

Model* modelSpiral = createModel<Spiral, SpiralWidget>("mod2-spiral");
