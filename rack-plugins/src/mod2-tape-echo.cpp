// WIP: Claude-generated maddie synths original (no upstream Hagiwo/Rob
// Scape firmware). Excluded from the VCV Rack build -- see WIP_SOURCES in
// the Makefile and the WIP block in plugin.cpp for how to re-enable it.
#include "plugin.hpp"
#include <TapeEchoCore.h>  // Shared tape DSP (also used by mod2-tape-echo firmware)

/*
	Tape Echo — worn-tape delay.

	Port of firmwares/mod2-tape-echo/mod2-tape-echo.ino (HAGIWO Mod2, RP2350).

	Mirrors the Mod2 hardware: 3 pots, 1 push button, 1 PWM LED, and the Mod2
	jack set (IN1, IN2, CV, OUT). For the Tape Echo firmware:
		POT1 (A0) -> Delay time (30 ms - 2 s), glides with tape-speed pitch bend
		POT2 (A1) -> Tape age macro (wow/flutter + HF loss + saturation + dropouts)
		POT3 (A2) -> unavailable on hardware (pin doubles as the audio input)
		BUTTON    -> tap tempo
		LED       -> tape health (flickers with flutter and dropouts)
		IN1       -> tap tempo / clock in
		IN2       -> splice gate (>1 V = tape-stop lurch)
		CV        -> AUDIO INPUT (sampled at the audio rate on hardware)
		OUT       -> audio output

	Deviations from hardware: the firmware's wet/dry and feedback live on
	shift layers (hold BUTTON + turn POT1/POT2) because POT3's pin is the
	audio input; here the physically-present-but-dead third knob becomes a
	proper Mix control and Feedback gets a trimmer beside the button (a spot
	that is empty on the real panel). The button is a plain tap-tempo button
	(every press is a tap — no shift layer to dodge). The tape arena is sized
	for Rack's engine rate, so the full 2 s (+ tape-stop headroom) is
	available at any sample rate.
*/

struct TapeEcho : Module {
	enum ParamId {
		TIME_PARAM,
		AGE_PARAM,
		MIX_PARAM,
		FEEDBACK_PARAM,  // trimmer — shift-layer param on hardware
		TAP_PARAM,       // momentary button — tap tempo
		PARAMS_LEN
	};
	enum InputId {
		TAP_INPUT,     // IN1 — tap tempo / clock
		SPLICE_INPUT,  // IN2 — splice gate (>1 V = tape stop)
		AUDIO_INPUT,   // CV jack — audio in
		INPUTS_LEN
	};
	enum OutputId {
		AUDIO_OUTPUT,  // OUT
		OUTPUTS_LEN
	};
	enum LightId {
		TAPE_LIGHT,
		LIGHTS_LEN
	};

	static constexpr float MAX_DELAY_SEC = 2.f;
	static constexpr float TAPE_SEC = 3.f;  // max delay + tape-stop headroom

	// Tape state lives in the shared core (same DSP as the firmware).
	sc::TapeEchoCore core;
	std::vector<int16_t> arena;

	dsp::SchmittTrigger clockTrigger;
	dsp::BooleanTrigger tapButton;
	float clockTimer = 0.f;    // seconds since the last IN1 edge
	float clockPeriod = 0.f;   // measured IN1 period (0 = none yet)
	float tapTimer = 1e6f;     // seconds since the last button tap
	float tapTimeSec = 0.f;    // 0 = no tap override active
	float lastTimeKnob = -1.f; // moving the knob reclaims it from the tap

	TapeEcho() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(TIME_PARAM, 0.f, 1.f, 0.5f, "Delay time (30 ms → 2 s, tape-speed glide)");
		configParam(AGE_PARAM, 0.f, 1.f, 0.3f, "Tape age (wow/flutter, HF loss, saturation, dropouts)", "%", 0.f, 100.f);
		configParam(MIX_PARAM, 0.f, 1.f, 0.5f, "Wet/dry mix", "%", 0.f, 100.f);
		configParam(FEEDBACK_PARAM, 0.f, 1.f, 0.45f, "Feedback (soft-limited past 100%)", "%", 0.f, 110.f);
		configButton(TAP_PARAM, "Tap tempo");

		configInput(TAP_INPUT, "IN1 tap tempo / clock");
		configInput(SPLICE_INPUT, "IN2 splice gate (>1 V = tape stop)");
		configInput(AUDIO_INPUT, "Audio");
		configOutput(AUDIO_OUTPUT, "Audio");

		configBypass(AUDIO_INPUT, AUDIO_OUTPUT);
		updateArena(APP->engine->getSampleRate());
	}

	// The int16 arena mirrors the firmware's SRAM buffer; the extra second
	// past MAX_DELAY_SEC is what the read head consumes during a tape stop.
	void updateArena(float fs) {
		const uint32_t n = (uint32_t)(TAPE_SEC * fs) + 16;
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
		tapTimeSec = 0.f;
		clockPeriod = 0.f;
	}

	void process(const ProcessArgs& args) override {
		// Button tap tempo: the interval between the last two presses.
		tapTimer += args.sampleTime;
		if (tapButton.process(params[TAP_PARAM].getValue() > 0.5f)) {
			if (tapTimer >= 0.030f && tapTimer <= MAX_DELAY_SEC)
				tapTimeSec = tapTimer;
			tapTimer = 0.f;
		}

		// IN1 tap/clock: each measured period sets the time directly.
		if (inputs[TAP_INPUT].isConnected()) {
			clockTimer += args.sampleTime;
			if (clockTrigger.process(inputs[TAP_INPUT].getVoltage(), 0.1f, 1.f)) {
				if (clockTimer < TAPE_SEC)
					clockPeriod = clockTimer;
				clockTimer = 0.f;
			}
		}
		else {
			clockTimer = 0.f;
			clockPeriod = 0.f;
		}

		// Moving the time knob reclaims it from any tap override.
		const float timeKnob = params[TIME_PARAM].getValue();
		if (lastTimeKnob < 0.f)
			lastTimeKnob = timeKnob;
		if (std::fabs(timeKnob - lastTimeKnob) > 0.01f) {
			tapTimeSec = 0.f;
			lastTimeKnob = timeKnob;
		}

		if (clockPeriod > 0.f)
			core.timeSec = clamp(clockPeriod, 0.030f, MAX_DELAY_SEC);
		else if (tapTimeSec > 0.f)
			core.timeSec = tapTimeSec;
		else
			core.timeSec = sc::tapeEchoTimeSec(timeKnob, MAX_DELAY_SEC);

		core.age = params[AGE_PARAM].getValue();
		core.feedback = sc::tapeEchoFeedback(params[FEEDBACK_PARAM].getValue());
		core.wet = params[MIX_PARAM].getValue();
		core.splice = inputs[SPLICE_INPUT].getVoltage() > 1.f;

		// +/-5 V -> -1..1 through the shared core and back.
		const float in = inputs[AUDIO_INPUT].getVoltage() / 5.f;
		const float out = core.process(in, args.sampleTime);
		outputs[AUDIO_OUTPUT].setVoltage(clamp(out, -1.f, 1.f) * 5.f);

		// LED shows the core's tape-health flicker, as on hardware.
		lights[TAPE_LIGHT].setBrightness(core.led);
	}
};

struct TapeEchoWidget : ModuleWidget {
	TapeEchoWidget(TapeEcho* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/mod2-tape-echo.svg")));
		// 4 HP Mod1/Mod2 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.03f, 21.7f)), module, TapeEcho::TIME_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 40.06f)), module, TapeEcho::AGE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 58.42f)), module, TapeEcho::MIX_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, TapeEcho::TAP_PARAM));
		// Feedback trimmer sits beside the tap button (empty on the real panel,
		// where feedback is BUTTON+POT2).
		addParam(createParamCentered<Trimpot>(mm2px(Vec(14.71f, 78.57f)), module, TapeEcho::FEEDBACK_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, TapeEcho::TAPE_LIGHT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, TapeEcho::TAP_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.3f)), module, TapeEcho::SPLICE_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, TapeEcho::AUDIO_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, TapeEcho::AUDIO_INPUT));
	}
};

Model* modelTapeEcho = createModel<TapeEcho, TapeEchoWidget>("mod2-tape-echo");
