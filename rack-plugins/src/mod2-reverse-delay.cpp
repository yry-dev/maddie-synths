// WIP: Claude-generated maddie synths original (no upstream Hagiwo/Rob
// Scape firmware). Excluded from the VCV Rack build -- see WIP_SOURCES in
// the Makefile and the WIP block in plugin.cpp for how to re-enable it.
#include "plugin.hpp"
#include <ReverseDelayCore.h>  // Shared reverse-delay DSP (also used by mod2-reverse-delay firmware)

/*
	Reverse Delay — classic ambient reverse echo.

	Port of firmwares/mod2-reverse-delay/mod2-reverse-delay.ino (HAGIWO Mod2, RP2350).

	Mirrors the Mod2 hardware: 3 pots, 1 push button, 1 PWM LED, and the Mod2
	jack set (IN1, IN2, CV, OUT). For the Reverse Delay firmware:
		POT1 (A0) -> Chunk time (100 ms - 2 s) / clock division when clocked
		POT2 (A1) -> Feedback (0 - 95%, re-reversing)
		POT3 (A2) -> unavailable on hardware (pin doubles as the audio input)
		BUTTON    -> mode: Reverse / Alternating / Octave-up reverse
		LED       -> ramps up across each reverse sweep (visualises the swell)
		IN1       -> clock in (chunk locks to musical divisions of the period)
		IN2       -> direction-flip gate (>1 V = forward "normal delay" pockets)
		CV        -> AUDIO INPUT (sampled at the audio rate on hardware)
		OUT       -> audio output

	Deviations from hardware: the firmware's wet/dry lives on a shift layer
	(hold BUTTON + turn POT1) because POT3's pin is the audio input; here the
	physically-present-but-dead third knob becomes a proper Mix control. The
	firmware's other shift layer (BUTTON + POT2 = grain "swell"/fade-in amount)
	has no free knob, so it is exposed on the right-click context menu (default
	0.35, as on the firmware); the button's long-press tap-tempo is dropped
	(patch a clock into IN1 instead). The reverse buffer is sized for Rack's
	engine rate, so the full 2 s chunk is available at any sample rate. The mode
	persists in the patch (firmware: flash).
*/

struct ReverseDelay : Module {
	enum ParamId {
		TIME_PARAM,
		FEEDBACK_PARAM,
		MIX_PARAM,
		MODE_PARAM,   // momentary button — cycles Reverse / Alternating / Octave
		PARAMS_LEN
	};
	enum InputId {
		CLOCK_INPUT,  // IN1 — clock sync
		FLIP_INPUT,   // IN2 — direction-flip gate (>1 V = forward pockets)
		AUDIO_INPUT,  // CV jack — audio in
		INPUTS_LEN
	};
	enum OutputId {
		AUDIO_OUTPUT, // OUT
		OUTPUTS_LEN
	};
	enum LightId {
		SWEEP_LIGHT,
		LIGHTS_LEN
	};

	// Reverse-delay state lives in the shared core (same DSP as the firmware).
	sc::ReverseDelayCore core;
	std::vector<int16_t> arena;

	dsp::SchmittTrigger clockTrigger;
	dsp::BooleanTrigger modeButton;
	float clockTimer = 0.f;   // seconds since the last IN1 edge
	float clockPeriod = 0.f;  // measured IN1 period (0 = none yet)
	float swell = 0.35f;      // grain fade-in amount (firmware: BUTTON + POT2)

	ReverseDelay() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(TIME_PARAM, 0.f, 1.f, 0.5f, "Chunk time (100 ms → 2 s; clocked: division)");
		configParam(FEEDBACK_PARAM, 0.f, 1.f, 0.4f, "Feedback", "%", 0.f, 95.f);
		configParam(MIX_PARAM, 0.f, 1.f, 0.5f, "Wet/dry mix", "%", 0.f, 100.f);
		configButton(MODE_PARAM, "Mode (Reverse / Alternating / Octave-up)");

		configInput(CLOCK_INPUT, "IN1 clock (locks chunk time to musical divisions)");
		configInput(FLIP_INPUT, "IN2 direction-flip gate (>1 V = forward pockets)");
		configInput(AUDIO_INPUT, "Audio");
		configOutput(AUDIO_OUTPUT, "Audio");

		configBypass(AUDIO_INPUT, AUDIO_OUTPUT);
		updateArena(APP->engine->getSampleRate());
	}

	// The int16 arena mirrors the firmware's SRAM buffer (2 x max chunk, since
	// reversed reads reach ~2x the chunk time behind the write head), sized for
	// the engine rate so the full 2 s chunk is always available.
	void updateArena(float fs) {
		const uint32_t n = (uint32_t)(2.f * sc::kReverseMaxChunkSec * fs) + 16;
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
		core.mode = sc::REVDLY_REVERSE;
		swell = 0.35f;
	}

	// Mode + swell persist with the patch (the firmware stores them in flash).
	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "reverseMode", json_integer(core.mode));
		json_object_set_new(rootJ, "swell", json_real(swell));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* modeJ = json_object_get(rootJ, "reverseMode");
		if (modeJ)
			core.mode = (uint8_t)clamp((int)json_integer_value(modeJ), 0,
			                           sc::REVDLY_MODE_COUNT - 1);
		json_t* swellJ = json_object_get(rootJ, "swell");
		if (swellJ)
			swell = clamp((float)json_real_value(swellJ), 0.f, 1.f);
	}

	void process(const ProcessArgs& args) override {
		if (modeButton.process(params[MODE_PARAM].getValue() > 0.5f))
			core.mode = (core.mode + 1) % sc::REVDLY_MODE_COUNT;

		// IN1 patched = clock sync: the time knob picks a musical ratio of the
		// measured period, exactly as on hardware.
		if (inputs[CLOCK_INPUT].isConnected()) {
			clockTimer += args.sampleTime;
			if (clockTrigger.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f)) {
				if (clockTimer < sc::kReverseMaxChunkSec * 4.f)
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
			core.chunkSec = clamp(clockPeriod * sc::reverseClockRatio(timeKnob),
			                      sc::kReverseMinChunkSec, sc::kReverseMaxChunkSec);
		else
			core.chunkSec = sc::reverseChunkSec(timeKnob);

		core.feedback = sc::reverseFeedback(params[FEEDBACK_PARAM].getValue());
		core.wet = params[MIX_PARAM].getValue();
		core.swell = swell;
		core.flip = inputs[FLIP_INPUT].getVoltage() > 1.f;

		// +/-5 V -> -1..1 through the shared core and back.
		const float in = inputs[AUDIO_INPUT].getVoltage() / 5.f;
		const float out = core.process(in, args.sampleTime);
		outputs[AUDIO_OUTPUT].setVoltage(clamp(out, -1.f, 1.f) * 5.f);

		// LED ramps up across each reverse sweep, as on hardware.
		lights[SWEEP_LIGHT].setBrightness(core.ledLevel());
	}
};

struct ReverseDelayWidget : ModuleWidget {
	ReverseDelayWidget(ReverseDelay* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/mod2-reverse-delay.svg")));
		// 4 HP Mod1/Mod2 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.03f, 21.7f)), module, ReverseDelay::TIME_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 40.06f)), module, ReverseDelay::FEEDBACK_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 58.42f)), module, ReverseDelay::MIX_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, ReverseDelay::MODE_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, ReverseDelay::SWEEP_LIGHT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, ReverseDelay::CLOCK_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.3f)), module, ReverseDelay::FLIP_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, ReverseDelay::AUDIO_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, ReverseDelay::AUDIO_INPUT));
	}

	void appendContextMenu(Menu* menu) override {
		ReverseDelay* module = getModule<ReverseDelay>();
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Swell (grain fade-in)"));
		menu->addChild(new SwellSlider(module));
	}

	// Firmware's BUTTON + POT2 "swell" shift layer, exposed as a menu slider.
	struct SwellSlider : ui::Slider {
		SwellSlider(ReverseDelay* module) {
			box.size.x = 180.f;
			quantity = new SwellQuantity(module);
		}
		~SwellSlider() { delete quantity; }
	};

	struct SwellQuantity : Quantity {
		ReverseDelay* module;
		SwellQuantity(ReverseDelay* m) : module(m) {}
		void setValue(float v) override { if (module) module->swell = clamp(v, 0.f, 1.f); }
		float getValue() override { return module ? module->swell : 0.35f; }
		float getDefaultValue() override { return 0.35f; }
		float getMinValue() override { return 0.f; }
		float getMaxValue() override { return 1.f; }
		std::string getLabel() override { return "Swell"; }
		std::string getUnit() override { return "%"; }
		float getDisplayValue() override { return getValue() * 100.f; }
		void setDisplayValue(float v) override { setValue(v / 100.f); }
	};
};

Model* modelReverseDelay = createModel<ReverseDelay, ReverseDelayWidget>("mod2-reverse-delay");
