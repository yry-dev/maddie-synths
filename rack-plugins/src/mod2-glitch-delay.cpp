// WIP: Claude-generated maddie synths original (no upstream Hagiwo/Rob
// Scape firmware). Excluded from the VCV Rack build -- see WIP_SOURCES in
// the Makefile and the WIP block in plugin.cpp for how to re-enable it.
#include "plugin.hpp"
#include <GlitchDelayCore.h>  // Shared glitch DSP (also used by mod2-glitch-delay firmware)

/*
	Glitch Delay — random skips, reverse chunks, stutters and tape-cuts.

	Port of firmwares/mod2-glitch-delay/mod2-glitch-delay.ino (HAGIWO Mod2, RP2350).

	Mirrors the Mod2 hardware: 3 pots, 1 push button, 1 PWM LED, and the Mod2
	jack set (IN1, IN2, CV, OUT). For the Glitch Delay firmware:
		POT1 (A0) -> Delay time / chunk length (20 ms - 4 s) / clock division
		POT2 (A1) -> Chaos (glitch probability + intensity; 0 = clean delay)
		POT3 (A2) -> unavailable on hardware (pin doubles as the audio input)
		BUTTON    -> glitch palette: Skips / Reverse / Stutter / All
		LED       -> flashes on each glitch event
		IN1       -> clock in (chunk boundaries lock to musical divisions)
		IN2       -> force-glitch gate (>1 V = guaranteed event)
		CV        -> AUDIO INPUT (sampled at the audio rate on hardware)
		OUT       -> audio output

	Deviations from hardware: the firmware's two shift layers (hold BUTTON +
	turn POT1 = wet/dry, + turn POT2 = feedback) exist because POT3's pin is the
	audio input. Here the physically-present-but-dead third knob becomes a
	proper Mix control; feedback — with no third knob to spare — lives in the
	right-click menu (saved with the patch). The firmware's long-press tap-tempo
	is dropped (patch a clock into IN1 instead). The delay buffer is sized for
	Rack's engine rate, so 4 s is available at any sample rate. The palette,
	feedback and the deterministic-loop toggle persist in the patch (firmware:
	flash).
*/

struct GlitchDelay : Module {
	enum ParamId {
		TIME_PARAM,
		CHAOS_PARAM,
		MIX_PARAM,
		FEEDBACK_PARAM,  // no panel knob — exposed in the right-click menu
		PALETTE_PARAM,   // momentary button — cycles Skips / Reverse / Stutter / All
		PARAMS_LEN
	};
	enum InputId {
		CLOCK_INPUT,  // IN1 — clock sync (quantises chunk boundaries)
		FORCE_INPUT,  // IN2 — force-glitch gate (>1 V = guaranteed event)
		AUDIO_INPUT,  // CV jack — audio in
		INPUTS_LEN
	};
	enum OutputId {
		AUDIO_OUTPUT, // OUT
		OUTPUTS_LEN
	};
	enum LightId {
		GLITCH_LIGHT,
		LIGHTS_LEN
	};

	static constexpr float MAX_DELAY_SEC = 4.f;

	// Glitch state lives in the shared core (same DSP as the firmware).
	sc::GlitchDelayCore core;
	std::vector<int16_t> arena;

	dsp::SchmittTrigger clockTrigger;
	dsp::BooleanTrigger paletteButton;
	float clockTimer = 0.f;   // seconds since the last IN1 edge
	float clockPeriod = 0.f;  // measured IN1 period (0 = none yet)

	GlitchDelay() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(TIME_PARAM, 0.f, 1.f, 0.4f, "Delay time / chunk length (20 ms → 4 s; clocked: division)");
		configParam(CHAOS_PARAM, 0.f, 1.f, 0.4f, "Chaos (glitch probability + intensity)", "%", 0.f, 100.f);
		configParam(MIX_PARAM, 0.f, 1.f, 0.5f, "Wet/dry mix", "%", 0.f, 100.f);
		configParam(FEEDBACK_PARAM, 0.f, 1.f, 0.4f, "Feedback", "%", 0.f, 100.f);
		configButton(PALETTE_PARAM, "Glitch palette (Skips / Reverse / Stutter / All)");

		configInput(CLOCK_INPUT, "IN1 clock (locks glitch chunks to musical divisions)");
		configInput(FORCE_INPUT, "IN2 force-glitch gate (>1 V = guaranteed event)");
		configInput(AUDIO_INPUT, "Audio");
		configOutput(AUDIO_OUTPUT, "Audio");

		configBypass(AUDIO_INPUT, AUDIO_OUTPUT);
		updateArena(APP->engine->getSampleRate());
	}

	// The int16 arena mirrors the firmware's SRAM buffer, sized for the
	// engine rate so the full 4 s is always available.
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
		core.palette = sc::GLITCH_ALL;
		core.deterministic = true;
	}

	// Palette / feedback / deterministic-loop persist with the patch (the
	// firmware stores palette + feedback in flash; feedback + Mix are params,
	// so they save automatically — palette and the toggle need JSON).
	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "palette", json_integer(core.palette));
		json_object_set_new(rootJ, "deterministic", json_boolean(core.deterministic));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* palJ = json_object_get(rootJ, "palette");
		if (palJ)
			core.palette = (uint8_t)clamp((int)json_integer_value(palJ), 0,
			                              sc::GLITCH_PALETTE_COUNT - 1);
		json_t* detJ = json_object_get(rootJ, "deterministic");
		if (detJ)
			core.deterministic = json_boolean_value(detJ);
	}

	void process(const ProcessArgs& args) override {
		if (paletteButton.process(params[PALETTE_PARAM].getValue() > 0.5f))
			core.palette = (core.palette + 1) % sc::GLITCH_PALETTE_COUNT;

		// IN1 patched = clock sync: the time knob picks a musical ratio of the
		// measured period for the chunk length, exactly as on hardware.
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
			core.timeSec = clamp(clockPeriod * sc::glitchDelayClockRatio(timeKnob),
			                     0.020f, MAX_DELAY_SEC);
		else
			core.timeSec = sc::glitchDelayTimeSec(timeKnob, MAX_DELAY_SEC);

		core.chaos = sc::glitchDelayChaos(params[CHAOS_PARAM].getValue());
		core.feedback = sc::glitchDelayFeedback(params[FEEDBACK_PARAM].getValue());
		core.wet = params[MIX_PARAM].getValue();
		core.force = inputs[FORCE_INPUT].getVoltage() > 1.f;

		// +/-5 V -> -1..1 through the shared core and back.
		const float in = inputs[AUDIO_INPUT].getVoltage() / 5.f;
		const float out = core.process(in, args.sampleTime);
		outputs[AUDIO_OUTPUT].setVoltage(clamp(out, -1.f, 1.f) * 5.f);

		// LED flashes on each glitch event, as on hardware.
		lights[GLITCH_LIGHT].setBrightness(core.ledLevel());
	}
};

struct GlitchDelayWidget : ModuleWidget {
	GlitchDelayWidget(GlitchDelay* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/mod2-glitch-delay.svg")));
		// 4 HP Mod1/Mod2 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.03f, 21.7f)), module, GlitchDelay::TIME_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 40.06f)), module, GlitchDelay::CHAOS_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 58.42f)), module, GlitchDelay::MIX_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, GlitchDelay::PALETTE_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, GlitchDelay::GLITCH_LIGHT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, GlitchDelay::CLOCK_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.3f)), module, GlitchDelay::FORCE_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, GlitchDelay::AUDIO_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, GlitchDelay::AUDIO_INPUT));
	}

	// Feedback has no spare knob on the 3-pot panel, so it lives here; the
	// deterministic-loop toggle rides along.
	void appendContextMenu(Menu* menu) override {
		GlitchDelay* module = getModule<GlitchDelay>();
		if (!module)
			return;

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Feedback"));
		ui::Slider* fbSlider = new ui::Slider;
		fbSlider->quantity = module->getParamQuantity(GlitchDelay::FEEDBACK_PARAM);
		fbSlider->box.size.x = 200.f;
		menu->addChild(fbSlider);

		menu->addChild(createBoolPtrMenuItem("Deterministic loop (repeatable glitches)", "",
		                                     &module->core.deterministic));
	}
};

Model* modelGlitchDelay = createModel<GlitchDelay, GlitchDelayWidget>("mod2-glitch-delay");
