#include "plugin.hpp"
#include <KickCore.h>  // Shared Kick voice (also used by mod2-kick firmware)

/*
	Kick — sine-wave kick drum with an exponential pitch sweep.

	Port of firmwares/mod2-kick/mod2-kick.ino (HAGIWO Mod2, RP2350).

	The firmware multiplexes 3 pots across 2 button-selected modes (and persists
	the 6 values in EEPROM). This port mirrors the Mod2 hardware: 3 knobs whose
	function the MODE button pages between two banks, with the LED lit on bank 2.
	All 6 params always drive the engine; the button only changes which three the
	panel knobs edit (the knobs jump to the other bank's values on switch):
		Mode 0:  PITCH (pitchMult)  SOFT CLIP        DECAY
		Mode 1:  START FREQ         END FREQ         CURVE
	Trigger comes from IN1 (the hardware has no manual-trigger button).

	The synthesis (sine + exponential pitch sweep f0->f1 shaped by CURVE, decay
	envelope, tanh soft-clip, raised-cosine tail fade) lives in the shared core
	KickCore.h. The firmware renders it into a table at the audio rate and plays
	it back; here we synthesize the same voice live at Rack's sample rate so it's
	sample-rate independent.

	Jacks mirror the Mod2 hardware used by this firmware:
		IN1    -> trigger input (rising edge)
		IN2    -> accent (high lowers the level by 50%, like the hardware)
		OUT    -> audio output
*/

struct Kick : Module {
	enum ParamId {
		PITCHMULT_PARAM,  // Mode 0 POT1
		SOFTCLIP_PARAM,   // Mode 0 POT2
		DECAY_PARAM,      // Mode 0 POT3
		STARTFREQ_PARAM,  // Mode 1 POT1
		ENDFREQ_PARAM,    // Mode 1 POT2
		CURVE_PARAM,      // Mode 1 POT3
		MODE_PARAM,       // button: page the 3 knobs between bank 0 / bank 1
		PARAMS_LEN
	};
	enum InputId {
		TRIG_INPUT,    // IN1
		ACCENT_INPUT,  // IN2
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

	// Voice state lives in the shared core (same synthesis as the firmware).
	sc::KickVoice voice;

	dsp::SchmittTrigger gateTrigger;
	dsp::BooleanTrigger modeTrigger;

	int mode = 0;  // 0 = bank PITCH/CLIP/DECAY, 1 = bank START/END/CURVE

	Kick() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		// Ranges match the firmware's pot mappings (engine units).
		configParam(PITCHMULT_PARAM, 0.5f, 2.0f, 1.0f, "Pitch mult", "x");
		configParam(SOFTCLIP_PARAM, 0.5f, 10.0f, 2.0f, "Soft clip");
		configParam(DECAY_PARAM, 1.0f, 10.0f, 5.0f, "Decay");
		configParam(STARTFREQ_PARAM, 3.0f, 1026.0f, 250.0f, "Start frequency", " Hz");
		configParam(ENDFREQ_PARAM, 2.0f, 512.0f, 50.0f, "End frequency", " Hz");
		configParam(CURVE_PARAM, 0.1f, 2.0f, 0.6f, "Pitch envelope curve");
		configButton(MODE_PARAM, "Mode (page knob bank)");

		configInput(TRIG_INPUT, "IN1 trigger");
		configInput(ACCENT_INPUT, "IN2 accent (high lowers level)");
		configOutput(AUDIO_OUTPUT, "Audio");
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "mode", json_integer(mode));
		return root;
	}
	void dataFromJson(json_t* root) override {
		if (json_t* m = json_object_get(root, "mode"))
			mode = json_integer_value(m);
	}

	// Sample knobs/accent and start a new strike (firmware onTrigger()).
	void strike() {
		voice.setParams(
			params[PITCHMULT_PARAM].getValue(),
			params[SOFTCLIP_PARAM].getValue(),
			params[DECAY_PARAM].getValue(),
			params[STARTFREQ_PARAM].getValue(),
			params[ENDFREQ_PARAM].getValue(),
			params[CURVE_PARAM].getValue());

		// Accent: IN2 high lowers the level by 50%, like the hardware.
		const float reduceLevel = inputs[ACCENT_INPUT].getVoltage() > 1.f ? 0.5f : 1.f;
		voice.strike(reduceLevel);
	}

	void onReset() override {
		voice.reset();
	}

	void process(const ProcessArgs& args) override {
		// Button pages the knob bank (firmware's mode button).
		if (modeTrigger.process(params[MODE_PARAM].getValue() > 0.5f))
			mode ^= 1;

		// Fire on IN1 rising edge.
		if (gateTrigger.process(inputs[TRIG_INPUT].getVoltage(), 0.1f, 1.f))
			strike();

		// One sample from the shared core: audio in -1..1, envelope in 0..1.
		const sc::KickFrame f = voice.process(args.sampleTime);
		outputs[AUDIO_OUTPUT].setVoltage(f.audio * 5.f); // -1..1 -> +/-5V (Vpp 10V)
		// LED: envelope, tinted brighter on bank 1 so the mode is visible.
		lights[ENV_LIGHT].setBrightnessSmooth(mode ? 1.f : f.env, args.sampleTime);
	}
};

struct KickWidget : ModuleWidget {
	RoundBlackKnob* knobs[3] = {};
	int shownMode = -1;
	const int bank0[3] = {Kick::PITCHMULT_PARAM, Kick::SOFTCLIP_PARAM, Kick::DECAY_PARAM};
	const int bank1[3] = {Kick::STARTFREQ_PARAM, Kick::ENDFREQ_PARAM, Kick::CURVE_PARAM};

	KickWidget(Kick* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/mod2-kick.svg")));

		// 4 HP Mod2 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		const Vec pot[3] = {mm2px(Vec(10.03f, 21.70f)), mm2px(Vec(10.04f, 40.06f)), mm2px(Vec(10.04f, 58.42f))};
		for (int i = 0; i < 3; i++) {
			knobs[i] = createParamCentered<RoundBlackKnob>(pot[i], module, bank0[i]);
			addParam(knobs[i]);
		}
		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, Kick::MODE_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, Kick::ENV_LIGHT));

		// Jacks: IN1 trigger (TL), IN2 accent (TR), audio OUT (BL); CV hole (BR) unused.
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, Kick::TRIG_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.30f)), module, Kick::ACCENT_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, Kick::AUDIO_OUTPUT));
	}

	// Page the three knobs to the active bank when the mode changes.
	void step() override {
		if (module) {
			int m = static_cast<Kick*>(module)->mode;
			if (m != shownMode) {
				const int* bank = m ? bank1 : bank0;
				for (int i = 0; i < 3; i++) {
					knobs[i]->paramId = bank[i];
					knobs[i]->initParamQuantity();
				}
				shownMode = m;
			}
		}
		ModuleWidget::step();
	}
};

Model* modelKick = createModel<Kick, KickWidget>("mod2-kick");
