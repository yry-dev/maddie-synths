#include "plugin.hpp"
#include <FmDrumCore.h>  // Shared FM-drum voice (also used by the mod2-fm-drum firmware)

/*
	FMDrum — two-operator FM percussion voice.

	Port of firmwares/mod2-fm-drum/mod2-fm-drum.ino (HAGIWO Mod2, RP2350).

	Faithful to the Mod2 hardware: three pots multiplexed across two
	button-selected modes (POT3 "mod index" is shared by both), matching the
	panel (3 pots, 1 button, 1 LED, 4 jacks):

		Mode 0  POT1 -> Pitch        POT2 -> Op ratio     POT3 -> Mod index
		Mode 1  POT1 -> Decay        POT2 -> Ratio env    POT3 -> Mod index

		BUTTON -> toggle Mode 0 / Mode 1
		LED    -> on in Mode 1 (off in Mode 0)
		IN1    -> trigger          IN2 -> accent (HIGH cuts -6 dB)
		CV     -> mod index (shared with POT3 on hardware)
		OUT    -> audio

	The mode switch uses the firmware's "pickup" behaviour: when you toggle modes
	the two multiplexed knobs do not jump — each grabs its stored value only once
	you sweep the knob through it. Both modes' values persist (saved in the patch).
	The active mode is also selectable in the right-click menu.

	Deviation: knob directions are non-inverted (CW = more); the firmware inverts
	some pots purely as a hardware-pickup choice. The FM synthesis is the shared
	sc::FmDrumVoice core, identical to the firmware.
*/

// One physical knob that stores two values (Mode 0 = a, Mode 1 = b) and uses
// pickup so toggling modes never jumps the parameter.
struct DualKnob {
	float a, b;          // stored value per mode
	float target = 0.f;  // value to pick up after a mode change
	bool picked = true;  // true once the knob has grabbed the active value

	DualKnob(float a0 = 0.5f, float b0 = 0.5f) : a(a0), b(b0) {}

	// Arm pickup for the mode just switched to.
	void arm(bool mode1) { picked = false; target = mode1 ? b : a; }

	// Feed the live knob position; stores into the active mode once picked up.
	void update(float knob, bool mode1) {
		if (!picked) {
			if (std::fabs(knob - target) <= 0.02f) picked = true;
			else return; // hold stored values until the knob reaches them
		}
		if (mode1) b = knob; else a = knob;
	}
};

struct FMDrum : Module {
	enum ParamId {
		KNOB1_PARAM,  // Pitch (Mode 0) / Decay (Mode 1)
		KNOB2_PARAM,  // Op ratio (Mode 0) / Ratio env (Mode 1)
		KNOB3_PARAM,  // Mod index (both modes)
		MODE_PARAM,   // button — toggles mode
		PARAMS_LEN
	};
	enum InputId {
		TRIG_INPUT,     // IN1
		ACCENT_INPUT,   // IN2 (HIGH = -6 dB)
		INDEX_CV_INPUT, // CV (shared with POT3 on hardware)
		INPUTS_LEN
	};
	enum OutputId {
		AUDIO_OUTPUT,   // OUT
		OUTPUTS_LEN
	};
	enum LightId {
		MODE_LIGHT,     // on in Mode 1
		LIGHTS_LEN
	};

	// Voice state lives in the shared core (same synthesis as the firmware).
	sc::FmDrumVoice voice;

	dsp::SchmittTrigger gateTrigger;
	dsp::BooleanTrigger modeButton;

	bool mode1 = false;                  // false = Mode 0, true = Mode 1
	DualKnob knob1{0.15f, 0.5f};         // pitch / decay
	DualKnob knob2{0.2f, 0.f};           // op ratio / ratio env

	FMDrum() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(KNOB1_PARAM, 0.f, 1.f, 0.15f, "Pitch / Decay");
		configParam(KNOB2_PARAM, 0.f, 1.f, 0.2f, "Op ratio / Ratio env");
		configParam(KNOB3_PARAM, 0.f, 1.f, 0.3f, "Modulation index");
		configButton(MODE_PARAM, "Mode (off = Pitch/Ratio/Index, on = Decay/RatioEnv/Index)");

		configInput(TRIG_INPUT, "IN1 trigger");
		configInput(ACCENT_INPUT, "Accent (-6 dB when high)");
		configInput(INDEX_CV_INPUT, "Mod index CV");
		configOutput(AUDIO_OUTPUT, "Audio");
	}

	// Sample knobs/CV/accent and start a new strike (firmware onTrigger() bake).
	void strike() {
		const float f0 = 30.f + 1170.f * knob1.a;                              // 30..1200 Hz (Pitch)
		const float opRatio = 0.5f + 7.5f * knob2.a;                           // 0.5..8 (Op ratio)
		const float modIndex = clamp(1.f + 9.f * params[KNOB3_PARAM].getValue()
		                             + inputs[INDEX_CV_INPUT].getVoltage(), 1.f, 10.f);
		const float decayRate = 0.5f + 9.5f * knob1.b;                         // 0.5..10 (Decay)
		const float ratioEnv = knob2.b;                                        // 0..1 (Ratio env)
		const float accentLevel = inputs[ACCENT_INPUT].getVoltage() > 1.f ? 0.5f : 1.f;

		voice.setParams(f0, opRatio, modIndex, decayRate, ratioEnv,
		                sc::kFmDrumIndexEnv, accentLevel);
		voice.strike();
	}

	void onReset() override {
		voice.reset();
		mode1 = false;
		knob1 = DualKnob{0.15f, 0.5f};
		knob2 = DualKnob{0.2f, 0.f};
	}

	void setMode(bool m) {
		if (m == mode1) return;
		mode1 = m;
		knob1.arm(mode1);
		knob2.arm(mode1);
	}

	void process(const ProcessArgs& args) override {
		// Button toggles mode; the multiplexed knobs re-pickup their stored value.
		if (modeButton.process(params[MODE_PARAM].getValue() > 0.5f))
			setMode(!mode1);

		knob1.update(params[KNOB1_PARAM].getValue(), mode1);
		knob2.update(params[KNOB2_PARAM].getValue(), mode1);
		lights[MODE_LIGHT].setBrightness(mode1 ? 1.f : 0.f);

		// Fire on IN1 rising edge (the button is the mode toggle, not a trigger).
		if (gateTrigger.process(inputs[TRIG_INPUT].getVoltage(), 0.1f, 1.f))
			strike();

		// One sample from the shared core: audio in -1..1.
		const sc::FmDrumFrame f = voice.process(args.sampleTime);
		outputs[AUDIO_OUTPUT].setVoltage(f.audio * 5.f); // -1..1 -> +/-5V (Vpp 10V)
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "mode1", json_boolean(mode1));
		json_object_set_new(root, "pitch", json_real(knob1.a));
		json_object_set_new(root, "decay", json_real(knob1.b));
		json_object_set_new(root, "opRatio", json_real(knob2.a));
		json_object_set_new(root, "ratioEnv", json_real(knob2.b));
		return root;
	}

	void dataFromJson(json_t* root) override {
		if (json_t* j = json_object_get(root, "mode1")) mode1 = json_boolean_value(j);
		if (json_t* j = json_object_get(root, "pitch")) knob1.a = json_number_value(j);
		if (json_t* j = json_object_get(root, "decay")) knob1.b = json_number_value(j);
		if (json_t* j = json_object_get(root, "opRatio")) knob2.a = json_number_value(j);
		if (json_t* j = json_object_get(root, "ratioEnv")) knob2.b = json_number_value(j);
		// Knob positions reflect the active mode; pickup is satisfied on load.
		knob1.picked = knob2.picked = true;
	}
};

struct FMDrumWidget : ModuleWidget {
	FMDrumWidget(FMDrum* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/mod2-fm-drum.svg")));

		// 4 HP panel (19.8 mm): hole centres from the mod2-fm-drum KiCad faceplate
		// (panel-local mm, scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.03f, 21.70f)), module, FMDrum::KNOB1_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 40.06f)), module, FMDrum::KNOB2_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 58.42f)), module, FMDrum::KNOB3_PARAM));

		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, FMDrum::MODE_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, FMDrum::MODE_LIGHT));

		// Jacks: IN1 trigger (top-left), IN2 accent (top-right), OUT (bottom-left), CV (bottom-right).
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, FMDrum::TRIG_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.30f)), module, FMDrum::ACCENT_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, FMDrum::AUDIO_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, FMDrum::INDEX_CV_INPUT));
	}

	void appendContextMenu(Menu* menu) override {
		FMDrum* m = dynamic_cast<FMDrum*>(module);
		assert(m);
		menu->addChild(new MenuSeparator);
		menu->addChild(createIndexSubmenuItem("Mode",
			{"0: Pitch / Ratio / Index", "1: Decay / RatioEnv / Index"},
			[=]() { return m->mode1 ? 1 : 0; },
			[=](int i) { m->setMode(i == 1); }));
	}
};

Model* modelFMDrum = createModel<FMDrum, FMDrumWidget>("mod2-fm-drum");
