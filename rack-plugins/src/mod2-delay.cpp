#include "plugin.hpp"
#include <DelayFxCore.h>  // Shared delay DSP (also used by mod2-delay firmware)

/*
	Delay — clean/dirty mono digital delay.

	Port of firmwares/mod2-delay/mod2-delay.ino (HAGIWO Mod2, RP2350).

	Mirrors the Mod2 hardware: 3 pots, 1 push button, 1 PWM LED, and the Mod2
	jack set (IN1, IN2, CV, OUT). For the Delay firmware:
		POT1 (A0) -> Delay time (10 ms - 5 s) / clock division when clocked
		POT2 (A1) -> Feedback (0 - ~110%, soft-limited into self-oscillation)
		POT3 (A2) -> unavailable on hardware (pin doubles as the audio input)
		BUTTON    -> repeat colour: Clean / Dirty
		LED       -> blinks at the delay time
		IN1       -> clock in (delay locks to musical divisions of the period)
		IN2       -> hold gate (>1 V = infinite repeat, input muted)
		CV        -> AUDIO INPUT (sampled at the audio rate on hardware)
		OUT       -> audio output

	Deviations from hardware: the firmware's wet/dry lives on a shift layer
	(hold BUTTON + turn POT1) because POT3's pin is the audio input; here the
	physically-present-but-dead third knob becomes a proper Mix control, and
	the button's long-press tap-tempo is dropped (patch a clock into IN1
	instead). The delay buffer is sized for Rack's engine rate, so 5 s is
	available at any sample rate. The repeat colour persists in the patch
	(firmware: flash).
*/

struct Delay : Module {
	enum ParamId {
		TIME_PARAM,
		FEEDBACK_PARAM,
		MIX_PARAM,
		MODE_PARAM,   // momentary button — toggles Clean / Dirty
		PARAMS_LEN
	};
	enum InputId {
		CLOCK_INPUT,  // IN1 — clock sync
		HOLD_INPUT,   // IN2 — hold gate (>1 V = infinite repeat)
		AUDIO_INPUT,  // CV jack — audio in
		INPUTS_LEN
	};
	enum OutputId {
		AUDIO_OUTPUT, // OUT
		OUTPUTS_LEN
	};
	enum LightId {
		TIME_LIGHT,
		LIGHTS_LEN
	};

	static constexpr float MAX_DELAY_SEC = 5.f;

	// Delay state lives in the shared core (same DSP as the firmware).
	sc::DelayFxCore core;
	std::vector<int16_t> arena;

	dsp::SchmittTrigger clockTrigger;
	dsp::BooleanTrigger modeButton;
	float clockTimer = 0.f;   // seconds since the last IN1 edge
	float clockPeriod = 0.f;  // measured IN1 period (0 = none yet)
	float blinkTimer = 0.f;

	Delay() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(TIME_PARAM, 0.f, 1.f, 0.5f, "Delay time (10 ms → 5 s; clocked: division)");
		configParam(FEEDBACK_PARAM, 0.f, 1.f, 0.4f, "Feedback (soft-limited past 100%)", "%", 0.f, 110.f);
		configParam(MIX_PARAM, 0.f, 1.f, 0.5f, "Wet/dry mix", "%", 0.f, 100.f);
		configButton(MODE_PARAM, "Repeat colour (Clean / Dirty)");

		configInput(CLOCK_INPUT, "IN1 clock (locks time to musical divisions)");
		configInput(HOLD_INPUT, "IN2 hold gate (>1 V = infinite repeat)");
		configInput(AUDIO_INPUT, "Audio");
		configOutput(AUDIO_OUTPUT, "Audio");

		configBypass(AUDIO_INPUT, AUDIO_OUTPUT);
		updateArena(APP->engine->getSampleRate());
	}

	// The int16 arena mirrors the firmware's SRAM buffer, sized for the
	// engine rate so the full 5 s is always available.
	void updateArena(float fs) {
		const uint32_t n = (uint32_t)(MAX_DELAY_SEC * fs) + 16;
		if (arena.size() != n) {
			arena.assign(n, 0);
			core.init(arena.data(), n);
		}
	}

	void onSampleRateChange(const SampleRateChangeEvent& e) override {
		updateArena(e.sampleRate);
	}

	void onReset() override {
		core.reset();
		core.mode = sc::DELAYFX_CLEAN;
	}

	// Repeat colour persists with the patch (the firmware stores it in flash).
	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "repeatMode", json_integer(core.mode));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* modeJ = json_object_get(rootJ, "repeatMode");
		if (modeJ)
			core.mode = (uint8_t)clamp((int)json_integer_value(modeJ), 0,
			                           sc::DELAYFX_MODE_COUNT - 1);
	}

	void process(const ProcessArgs& args) override {
		if (modeButton.process(params[MODE_PARAM].getValue() > 0.5f))
			core.mode = (core.mode + 1) % sc::DELAYFX_MODE_COUNT;

		// IN1 patched = clock sync: the time knob picks a musical ratio of
		// the measured period, exactly as on hardware.
		if (inputs[CLOCK_INPUT].isConnected()) {
			clockTimer += args.sampleTime;
			if (clockTrigger.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f)) {
				if (clockTimer < MAX_DELAY_SEC)
					clockPeriod = clockTimer;
				clockTimer = 0.f;
			}
		}
		else {
			clockTimer = 0.f;
			clockPeriod = 0.f;
		}

		const float timeKnob = params[TIME_PARAM].getValue();
		if (clockPeriod > 0.f)
			core.timeSec = clamp(clockPeriod * sc::delayFxClockRatio(timeKnob),
			                     0.010f, MAX_DELAY_SEC);
		else
			core.timeSec = sc::delayFxTimeSec(timeKnob, MAX_DELAY_SEC);

		core.feedback = sc::delayFxFeedback(params[FEEDBACK_PARAM].getValue());
		core.wet = params[MIX_PARAM].getValue();
		core.hold = inputs[HOLD_INPUT].getVoltage() > 1.f;

		// +/-5 V -> -1..1 through the shared core and back.
		const float in = inputs[AUDIO_INPUT].getVoltage() / 5.f;
		const float out = core.process(in, args.sampleTime);
		outputs[AUDIO_OUTPUT].setVoltage(clamp(out, -1.f, 1.f) * 5.f);

		// LED blinks at the delay time, as on hardware.
		blinkTimer += args.sampleTime;
		if (blinkTimer >= core.timeSec)
			blinkTimer = 0.f;
		lights[TIME_LIGHT].setBrightness(blinkTimer < 0.05f ? 1.f : 0.f);
	}
};

struct DelayWidget : ModuleWidget {
	DelayWidget(Delay* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/mod2-delay.svg")));
		// 4 HP Mod1/Mod2 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.03f, 21.7f)), module, Delay::TIME_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 40.06f)), module, Delay::FEEDBACK_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 58.42f)), module, Delay::MIX_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, Delay::MODE_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, Delay::TIME_LIGHT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, Delay::CLOCK_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.3f)), module, Delay::HOLD_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, Delay::AUDIO_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, Delay::AUDIO_INPUT));
	}
};

Model* modelDelay = createModel<Delay, DelayWidget>("mod2-delay");
