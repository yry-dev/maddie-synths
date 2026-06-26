#include "plugin.hpp"
#include <FluxCore.h>  // Shared Flux voice (also used by mod2-flux firmware)

/*
	Flux — physical-modelling / resonance / noise multi-mode voice.

	Port of firmwares/mod2-flux/mod2-flux.ino (HAGIWO Mod2, RP2350).

	Seven modes in three groups (MODE knob):
		RESONANCE: 0 Modal (tuned resonator bank), 1 Karplus (plucked string)
		NOISE:     2 White, 3 Pink (1/f), 4 S&H (stepped), 5 Quantum (Lorenz)
		TEXTURE:   6 Drone (evolving harmonic texture)

	Controls (mirror the Mod2 hardware pots):
		FREQ   -> centre / pitch frequency
		RATE   -> auto-trigger speed / chaos rate
		CHAR   -> per-mode character / brightness / slew
		MODE   -> mode selector (snaps 0..6; the hardware cycles these with the button)
		BUTTON -> manual trigger / pluck (hardware long-press)
		IN1    -> trigger / gate (manual excite, rising edge)
		IN2    -> unused by this firmware
		CV     -> 1V/Oct pitch (see deviation below)
		OUT    -> audio output
		LED    -> blinks on every trigger (auto or manual)

	The Modal and Karplus modes self-trigger on the RATE control, and the Drone
	mode slowly re-randomises its harmonic balance — all driven inside the shared
	core so this port runs the same way the hardware does, while the BUTTON / IN1
	let you pluck on demand.

	Deviations from hardware:
	  - On the Mod2, mode is chosen by the panel button (short press cycles); here
	    it is a dedicated MODE knob so every mode is directly reachable.
	  - On the Mod2 the CV jack is wired to the character pot (POT3). This port
	    routes CV to 1V/Oct pitch instead, since a pitched voice is far more useful
	    in a rack; CHARACTER stays on its own knob.
*/

struct Flux : Module {
	enum ParamId {
		FREQ_PARAM,
		RATE_PARAM,
		CHAR_PARAM,
		MODE_PARAM,
		TRIG_PARAM,
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

	// Voice state lives in the shared core (same synthesis as the firmware).
	sc::FluxVoice voice;

	dsp::SchmittTrigger gateTrigger;
	dsp::BooleanTrigger buttonTrigger;
	dsp::PulseGenerator ledPulse;

	Flux() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(FREQ_PARAM, 0.f, 1.f, 0.5f, "Frequency");
		configParam(RATE_PARAM, 0.f, 1.f, 0.5f, "Rate / chaos rate");
		configParam(CHAR_PARAM, 0.f, 1.f, 0.5f, "Character / brightness / slew");
		configSwitch(MODE_PARAM, 0.f, 6.f, 0.f, "Mode",
			{"Modal", "Karplus", "White", "Pink", "S&H", "Quantum", "Drone"});
		configButton(TRIG_PARAM, "Manual trigger / pluck");

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

		// Fire on IN1 rising edge or a button press.
		const bool gate = gateTrigger.process(inputs[TRIG_INPUT].getVoltage(), 0.1f, 1.f);
		const bool button = buttonTrigger.process(params[TRIG_PARAM].getValue() > 0.5f);
		if (gate || button)
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
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Flux.svg")));

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		const float cx = 20.32f;  // 8 HP center

		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(cx, 15.f)), module, Flux::TRIG_LIGHT));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(cx, 27.f)), module, Flux::FREQ_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(cx, 43.f)), module, Flux::RATE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(cx, 59.f)), module, Flux::CHAR_PARAM));
		addParam(createParamCentered<RoundBlackSnapKnob>(mm2px(Vec(cx, 75.f)), module, Flux::MODE_PARAM));

		addParam(createParamCentered<VCVButton>(mm2px(Vec(cx, 89.f)), module, Flux::TRIG_PARAM));

		// Jacks: row 1 inputs (TRIG, V/Oct), row 2 IN2 + OUT.
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(13.5f, 102.f)), module, Flux::TRIG_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(27.14f, 102.f)), module, Flux::CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(13.5f, 116.f)), module, Flux::IN2_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(27.14f, 116.f)), module, Flux::AUDIO_OUTPUT));
	}
};

Model* modelFlux = createModel<Flux, FluxWidget>("Flux");
