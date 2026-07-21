// WIP: Claude-generated maddie synths original (no upstream Hagiwo/Rob
// Scape firmware). Excluded from the VCV Rack build -- see WIP_SOURCES in
// the Makefile and the WIP block in plugin.cpp for how to re-enable it.
#include "plugin.hpp"
#include <FreezeCore.h>  // Shared freeze DSP (also used by mod2-freeze firmware)

/*
	Freeze — buffer capture & seamless loop.

	Port of firmwares/mod2-freeze/mod2-freeze.ino (HAGIWO Mod2, RP2350).

	Mirrors the Mod2 hardware: 3 pots, 1 push button, 1 PWM LED, and the Mod2
	jack set (IN1, IN2, CV, OUT). For the Freeze firmware:
		POT1 (A0) -> Loop length (5 ms - 5.5 s, exponential taper)
		POT2 (A1) -> Loop window position (0 = newest slice, 1 = oldest)
		POT3 (A2) -> unavailable on hardware (pin doubles as the audio input)
		BUTTON    -> playback mode: Forward / Ping-pong / Half-speed
		LED       -> solid when frozen, follows the input level while live
		IN1       -> freeze gate (>1 V = freeze while high)
		IN2       -> re-capture trigger (grab fresh audio without unfreezing)
		CV        -> AUDIO INPUT (sampled at the audio rate on hardware)
		OUT       -> audio output

	Deviations from hardware: the firmware's frozen/live mix lives on a shift
	layer (hold BUTTON + turn POT1) because POT3's pin is the audio input; here
	the physically-present-but-dead third knob becomes a proper Mix control. The
	firmware's second shift-layer parameter (seam crossfade length, BUTTON+POT2)
	and its manual freeze latch (BUTTON long press) are dropped — the crossfade
	uses the firmware's default and freeze is driven by the IN1 gate. The capture
	buffer is sized for Rack's engine rate, so the full ~5.5 s is available at any
	sample rate. The playback mode persists in the patch (firmware: flash).
*/

struct Freeze : Module {
	enum ParamId {
		LENGTH_PARAM,
		POSITION_PARAM,
		MIX_PARAM,
		MODE_PARAM,   // momentary button — cycles Forward / Ping-pong / Half-speed
		PARAMS_LEN
	};
	enum InputId {
		FREEZE_INPUT,     // IN1 — freeze gate (>1 V = freeze)
		RECAPTURE_INPUT,  // IN2 — re-capture trigger
		AUDIO_INPUT,      // CV jack — audio in
		INPUTS_LEN
	};
	enum OutputId {
		AUDIO_OUTPUT, // OUT
		OUTPUTS_LEN
	};
	enum LightId {
		FREEZE_LIGHT,
		LIGHTS_LEN
	};

	// Freeze state lives in the shared core (same DSP as the firmware).
	sc::FreezeCore core;
	std::vector<int16_t> arena;

	dsp::SchmittTrigger freezeGate;    // IN1 gate, hi/lo hysteresis
	dsp::SchmittTrigger recaptureTrig; // IN2 trigger edge
	dsp::BooleanTrigger modeButton;

	Freeze() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(LENGTH_PARAM, 0.f, 1.f, 0.4f, "Loop length (5 ms → 5.5 s)");
		configParam(POSITION_PARAM, 0.f, 1.f, 0.f, "Window position (0 newest → 1 oldest)", "%", 0.f, 100.f);
		configParam(MIX_PARAM, 0.f, 1.f, 1.f, "Frozen/live mix", "%", 0.f, 100.f);
		configButton(MODE_PARAM, "Playback (Forward / Ping-pong / Half-speed)");

		configInput(FREEZE_INPUT, "IN1 freeze gate (>1 V = freeze)");
		configInput(RECAPTURE_INPUT, "IN2 re-capture trigger");
		configInput(AUDIO_INPUT, "Audio");
		configOutput(AUDIO_OUTPUT, "Audio");

		configBypass(AUDIO_INPUT, AUDIO_OUTPUT);
		updateArena(APP->engine->getSampleRate());
	}

	// The int16 arena mirrors the firmware's SRAM ring, sized for the engine
	// rate so the full ~5.5 s of capture is available at any sample rate.
	void updateArena(float fs) {
		const uint32_t n = sc::freezeArenaSamples(fs);
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
		core.mode = sc::FREEZE_FORWARD;
	}

	// Playback mode persists with the patch (the firmware stores it in flash).
	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "playMode", json_integer(core.mode));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* modeJ = json_object_get(rootJ, "playMode");
		if (modeJ)
			core.mode = (uint8_t)clamp((int)json_integer_value(modeJ), 0,
			                           sc::FREEZE_MODE_COUNT - 1);
	}

	void process(const ProcessArgs& args) override {
		if (modeButton.process(params[MODE_PARAM].getValue() > 0.5f))
			core.mode = (core.mode + 1) % sc::FREEZE_MODE_COUNT;

		// Same pot mappings as the firmware.
		core.loopLen = sc::freezeLoopLenSec(params[LENGTH_PARAM].getValue(),
		                                    sc::kFreezeBufferSec);
		core.position = params[POSITION_PARAM].getValue();
		core.wet = params[MIX_PARAM].getValue();

		// IN1 gate freezes while high; IN2 edge grabs fresh audio.
		freezeGate.process(inputs[FREEZE_INPUT].getVoltage(), 0.1f, 1.f);
		core.freeze = freezeGate.isHigh();
		if (recaptureTrig.process(inputs[RECAPTURE_INPUT].getVoltage(), 0.1f, 1.f))
			core.recapture();

		// +/-5 V -> -1..1 through the shared core and back.
		const float in = inputs[AUDIO_INPUT].getVoltage() / 5.f;
		const float out = core.process(in, args.sampleTime);
		outputs[AUDIO_OUTPUT].setVoltage(clamp(out, -1.f, 1.f) * 5.f);

		// LED: solid when frozen, follows the input level while live.
		lights[FREEZE_LIGHT].setBrightness(core.ledLevel());
	}
};

struct FreezeWidget : ModuleWidget {
	FreezeWidget(Freeze* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/mod2-freeze.svg")));
		// 4 HP Mod1/Mod2 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.03f, 21.7f)), module, Freeze::LENGTH_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 40.06f)), module, Freeze::POSITION_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 58.42f)), module, Freeze::MIX_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, Freeze::MODE_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, Freeze::FREEZE_LIGHT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, Freeze::FREEZE_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.3f)), module, Freeze::RECAPTURE_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, Freeze::AUDIO_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, Freeze::AUDIO_INPUT));
	}
};

Model* modelFreeze = createModel<Freeze, FreezeWidget>("mod2-freeze");
