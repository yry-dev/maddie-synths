#include "plugin.hpp"
#include <Acid303Voice.h>  // Shared Acid303 voice + sequencer (also used by mod2-acid303 firmware)

/*
	Acid303 — generative 303-style acid bass voice + Turing-machine sequencer.

	Port of firmwares/mod2-acid303/mod2-acid303.ino (HAGIWO Mod2, RP2350).

	Mirrors the Mod2 hardware: 3 pots, 1 push button, 1 LED, and the Mod2 jack
	set (IN1, IN2, OUT, CV):
		POT1 (A0)  -> TURING: morphs a locked 8-step pattern (CCW) through fully
		              random/evolving pitch+rhythm (noon) to a locked 16-step
		              pattern (CW). Sets both step length and mutation probability.
		POT2 (A1)  -> DECAY: amp decay + bite (accent extends the decay).
		POT3 (A2)  -> TRANS: transpose, quantised to semitones (shared with CV).
		BUTTON     -> short = next scale, double-click = next waveform,
		              long-hold = regenerate the base pattern.
		LED        -> step / accent pulse (the amp envelope).
		IN1        -> clock in (rising edge advances the sequence).
		IN2        -> accent hold (high forces accents).
		CV         -> transpose CV, summed as 1V/Oct semitones with POT3.
		OUT        -> audio output.

	The synthesis and sequencing live in the shared core Acid303Voice.h. The
	firmware runs its audio ISR at 31250 Hz; here we synthesise live at the host
	sample rate, so it is sample-rate independent.

	When the clock input is unpatched the voice free-runs (like the firmware's
	no-clock demo mode), with the internal tempo taken from the TRANSPOSE knob.

	Convergences (see Acid303Voice.h): deterministic xorshift PRNG instead of
	Arduino random(); the subtle per-odd-step swing is omitted; transpose CV is
	summed as 1V/Oct semitones.
*/

struct Acid303 : Module {
	enum ParamId {
		TURING_PARAM,
		DECAY_PARAM,
		TRANSPOSE_PARAM,
		BUTTON_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		CLOCK_INPUT,   // IN1
		ACCENT_INPUT,  // IN2
		CV_INPUT,      // transpose CV
		INPUTS_LEN
	};
	enum OutputId {
		AUDIO_OUTPUT,  // OUT
		OUTPUTS_LEN
	};
	enum LightId {
		STEP_LIGHT,
		LIGHTS_LEN
	};

	sc::Acid303Voice voice;
	dsp::SchmittTrigger clockTrigger;

	// Single-button gesture decoder (mirrors the firmware's handleButton timing).
	float tSec = 0.f;          // monotonic seconds for button timing
	bool btnWasDown = false;
	float btnDownT = 0.f;
	float lastClickT = -1.f;
	int clickCount = 0;

	Acid303() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(TURING_PARAM, 0.f, 1.f, 0.5f, "Turing (length + randomness)");
		configParam(DECAY_PARAM, 0.f, 1.f, 0.5f, "Decay + bite");
		configParam(TRANSPOSE_PARAM, 0.f, 1.f, 0.5f, "Transpose", " semitones", 0.f, 24.f, -12.f);
		configButton(BUTTON_PARAM, "Short: scale · Double: wave · Hold: regenerate");

		configInput(CLOCK_INPUT, "IN1 clock (rising advances)");
		configInput(ACCENT_INPUT, "IN2 accent (high forces accents)");
		configInput(CV_INPUT, "Transpose CV (1V/Oct)");
		configOutput(AUDIO_OUTPUT, "Audio");
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "scaleMode", json_integer(voice.scaleMode));
		json_object_set_new(root, "waveMode", json_integer(voice.waveMode));
		return root;
	}
	void dataFromJson(json_t* root) override {
		if (json_t* s = json_object_get(root, "scaleMode"))
			voice.scaleMode = json_integer_value(s);
		if (json_t* w = json_object_get(root, "waveMode"))
			voice.waveMode = json_integer_value(w);
	}

	void onReset() override {
		voice.reset();
	}

	// Decode the single push button into the firmware's three gestures:
	// short press = next scale, double-click = next waveform, long hold = regen.
	void handleButton(float dt) {
		tSec += dt;
		bool down = params[BUTTON_PARAM].getValue() > 0.5f;

		if (down && !btnWasDown) {
			btnDownT = tSec;
			btnWasDown = true;
		} else if (!down && btnWasDown) {
			float held = tSec - btnDownT;
			btnWasDown = false;
			if (held > 0.520f) {
				voice.regen();
				clickCount = 0;
				return;
			}
			if (lastClickT >= 0.f && (tSec - lastClickT) < 0.350f) clickCount++;
			else clickCount = 1;
			lastClickT = tSec;
			if (clickCount == 2) {
				voice.cycleWave();
				clickCount = 0;
			}
		}

		// A lone click that never became a double-click selects the next scale.
		if (clickCount == 1 && lastClickT >= 0.f && (tSec - lastClickT) > 0.380f) {
			voice.cycleScale();
			clickCount = 0;
		}
	}

	void process(const ProcessArgs& args) override {
		handleButton(args.sampleTime);

		// Controls
		voice.setTuring(params[TURING_PARAM].getValue());
		voice.decay01 = params[DECAY_PARAM].getValue();

		// Transpose: POT3 maps 0..1 -> -12..+12 semitones, plus CV as 1V/Oct.
		int potSemis = (int)std::round(rescale(params[TRANSPOSE_PARAM].getValue(), 0.f, 1.f, -12.f, 12.f));
		int cvSemis = (int)std::round(inputs[CV_INPUT].getVoltage() * 12.f);
		voice.setTranspose(clamp(potSemis + cvSemis, -48, 48));

		voice.accentHold = inputs[ACCENT_INPUT].getVoltage() > 1.f;

		// Free-run when no clock is patched; internal tempo from the TRANSPOSE
		// knob (60..220 ms), matching the firmware's no-clock demo mode.
		voice.freeRun = !inputs[CLOCK_INPUT].isConnected();
		voice.internalInterval = rescale(params[TRANSPOSE_PARAM].getValue(), 0.f, 1.f, 0.060f, 0.220f);

		// External clock edge advances the sequence.
		if (clockTrigger.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f))
			voice.clock();

		float out = voice.process(args.sampleTime);
		outputs[AUDIO_OUTPUT].setVoltage(out * 5.f);  // -1..1 -> ±5 V
		lights[STEP_LIGHT].setBrightnessSmooth(voice.lightLevel(), args.sampleTime);
	}
};

struct Acid303Widget : ModuleWidget {
	Acid303Widget(Acid303* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/mod2-acid303.svg")));

		// 4 HP panel (19.8 mm): hole centres from the mod2-acid303 KiCad faceplate
		// (panel-local mm, scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.03f, 21.70f)), module, Acid303::TURING_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 40.06f)), module, Acid303::DECAY_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 58.42f)), module, Acid303::TRANSPOSE_PARAM));

		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, Acid303::BUTTON_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, Acid303::STEP_LIGHT));

		// Jacks: IN1 clock (top-left), IN2 accent (top-right), OUT (bottom-left),
		// CV transpose (bottom-right).
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, Acid303::CLOCK_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.30f)), module, Acid303::ACCENT_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, Acid303::AUDIO_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, Acid303::CV_INPUT));
	}
};

Model* modelAcid303 = createModel<Acid303, Acid303Widget>("mod2-acid303");
