#include "plugin.hpp"
#include <SpiralCore.h>  // Shared Spiral synthesis (also used by mod2-spiral firmware)

/*
	Spiral 4Ever — auditory-illusion / Shepard-tone multi-mode generator.

	Port of firmwares/mod2-spiral/mod2-spiral.ino (HAGIWO Mod2, RP2350).

	Nine modes (selected by the MODE switch, mirroring the firmware's button
	cycle):
		0 Shepard rising   1 Shepard falling   2 Barber pole
		3 Risset rhythm    4 Tritone paradox   5 Tritone explorer
		6 Shepard cluster maj   7 Shepard cluster min   8 Euler spiral

	Controls mirror the Mod2 pots:
		FREQ  (POT1/A0) -> window centre frequency, exponential (0..5 "octaves")
		SPEED (POT2/A1) -> sweep speed
		WIDTH (POT3/A2) -> envelope width / pitch class / spiral spread
		MODE            -> mode select (firmware: short button press cycles)
		DIR             -> sweep direction (firmware: long button press toggles)
		CV              -> 1V/Oct added to the FREQ control
		OUT             -> audio output
		LED             -> lit when direction is "up"

	Deviation from hardware: on the Mod2 the single CV jack is wired to A2 (shared
	with POT3). This port routes CV to the window centre as 1V/Oct instead, which
	matches the firmware's documented "A0 -> Center frequency (V/oct)" intent and
	lets the illusion track a sequencer (the same musical choice the Claves port
	made for its pitch CV). The synthesis itself is byte-for-byte the shared core.
*/

struct Spiral : Module {
	enum ParamId {
		FREQ_PARAM,
		SPEED_PARAM,
		WIDTH_PARAM,
		MODE_PARAM,
		DIR_PARAM,
		PARAMS_LEN
	};
	enum InputId {
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

	// Synthesis state lives in the shared core (same DSP as the firmware).
	sc::SpiralCore voice;

	Spiral() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(FREQ_PARAM, 0.f, 1.f, 0.5f, "Center frequency");
		configParam(SPEED_PARAM, 0.f, 1.f, 0.4f, "Sweep speed");
		configParam(WIDTH_PARAM, 0.f, 1.f, 0.4f, "Width / pitch class / spread");
		configSwitch(MODE_PARAM, 0.f, 8.f, 0.f, "Mode", {
			"Shepard rising", "Shepard falling", "Barber pole",
			"Risset rhythm", "Tritone paradox", "Tritone explorer",
			"Shepard cluster maj", "Shepard cluster min", "Euler spiral"
		});
		configSwitch(DIR_PARAM, 0.f, 1.f, 1.f, "Direction", {"Down", "Up"});

		configInput(CV_INPUT, "1V/Oct (center frequency)");
		configOutput(AUDIO_OUTPUT, "Audio");
	}

	void onReset() override {
		voice.reset();
	}

	void process(const ProcessArgs& args) override {
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
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Spiral.svg")));

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		const float cx = 15.24f; // 6 HP center

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(cx, 24.f)), module, Spiral::FREQ_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(cx, 42.f)), module, Spiral::SPEED_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(cx, 60.f)), module, Spiral::WIDTH_PARAM));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(cx, 80.f)), module, Spiral::MODE_PARAM));

		addParam(createParamCentered<CKSS>(mm2px(Vec(cx, 95.f)), module, Spiral::DIR_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(cx + 8.f, 95.f)), module, Spiral::DIR_LIGHT));

		// Jacks: CV in (left), audio OUT (right).
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.16f, 113.f)), module, Spiral::CV_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(20.32f, 113.f)), module, Spiral::AUDIO_OUTPUT));
	}
};

Model* modelSpiral = createModel<Spiral, SpiralWidget>("Spiral");
