// WIP: Claude-generated maddie synths original (no upstream Hagiwo/Rob
// Scape firmware). Excluded from the VCV Rack build -- see WIP_SOURCES in
// the Makefile and the WIP block in plugin.cpp for how to re-enable it.
#include "plugin.hpp"
#include <StutterCore.h>  // Shared stutter DSP (also used by mod2-stutter firmware)

/*
	Stutter — clock-aware beat repeat.

	Port of firmwares/mod2-stutter/mod2-stutter.ino (HAGIWO Mod2, RP2350).

	Mirrors the Mod2 hardware: 3 pots, 1 push button, 1 PWM LED, and the Mod2
	jack set (IN1, IN2, CV, OUT). For the Stutter firmware:
		POT1 (A0) -> Repeat length (20 ms - 1 s free; clock division when clocked)
		POT2 (A1) -> Behaviour amount (decay / pitch-ramp per repeat)
		POT3 (A2) -> unavailable on hardware (pin doubles as the audio input)
		BUTTON    -> flavour: Straight / Decay / Pitch-up / Pitch-down
		LED       -> flashes on every repeat
		IN1       -> clock in (defines the musical grid)
		IN2       -> stutter gate (>1 V = repeats while high)
		CV        -> AUDIO INPUT (sampled at the audio rate on hardware)
		OUT       -> audio output

	Deviations from hardware: the firmware's wet/dry and probability live on
	shift layers (hold BUTTON + turn POT1/POT2) because POT3's pin is the audio
	input; here the physically-present-but-dead third knob becomes a proper Mix
	control, and probability gets a right-click menu entry (defaults to 1.0 =
	always). The firmware's long-press manual latch is dropped — hold IN2 high
	instead. The always-recording buffer is sized for Rack's engine rate. The
	flavour persists in the patch (firmware: flash).
*/

struct Stutter : Module {
	enum ParamId {
		LENGTH_PARAM,
		BEHAV_PARAM,
		MIX_PARAM,
		MODE_PARAM,   // momentary button — cycles the flavour
		PARAMS_LEN
	};
	enum InputId {
		CLOCK_INPUT,  // IN1 — clock sync (musical grid)
		GATE_INPUT,   // IN2 — stutter gate (>1 V = repeats)
		AUDIO_INPUT,  // CV jack — audio in
		INPUTS_LEN
	};
	enum OutputId {
		AUDIO_OUTPUT, // OUT
		OUTPUTS_LEN
	};
	enum LightId {
		REPEAT_LIGHT,
		LIGHTS_LEN
	};

	// Stutter state lives in the shared core (same DSP as the firmware).
	sc::StutterCore core;
	std::vector<int16_t> arena;

	dsp::SchmittTrigger clockTrigger;
	dsp::SchmittTrigger gateTrigger;
	dsp::BooleanTrigger modeButton;
	float clockTimer = 0.f;   // seconds since the last IN1 edge
	float clockPeriod = 0.f;  // measured IN1 period (0 = none yet)

	Stutter() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(LENGTH_PARAM, 0.f, 1.f, 0.4f, "Repeat length (20 ms → 1 s; clocked: division)");
		configParam(BEHAV_PARAM, 0.f, 1.f, 0.5f, "Behaviour (decay / pitch-ramp amount)", "%", 0.f, 100.f);
		configParam(MIX_PARAM, 0.f, 1.f, 1.f, "Wet/dry mix", "%", 0.f, 100.f);
		configButton(MODE_PARAM, "Flavour (Straight / Decay / Pitch up / Pitch down)");

		configInput(CLOCK_INPUT, "IN1 clock (defines the musical grid)");
		configInput(GATE_INPUT, "IN2 stutter gate (>1 V = repeats)");
		configInput(AUDIO_INPUT, "Audio");
		configOutput(AUDIO_OUTPUT, "Audio");

		configBypass(AUDIO_INPUT, AUDIO_OUTPUT);
		updateArena(APP->engine->getSampleRate());
	}

	// The int16 arena mirrors the firmware's SRAM buffer, sized for the
	// engine rate so the ~1.3 s capture window is identical at any rate.
	void updateArena(float fs) {
		const uint32_t n = sc::stutterArenaSamples(fs);
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
		core.mode = sc::STUTTER_STRAIGHT;
		core.probability = 1.f;
	}

	// Flavour + probability persist with the patch (the firmware stores them
	// in flash).
	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "stutterMode", json_integer(core.mode));
		json_object_set_new(rootJ, "probability", json_real(core.probability));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* modeJ = json_object_get(rootJ, "stutterMode");
		if (modeJ)
			core.mode = (uint8_t)clamp((int)json_integer_value(modeJ), 0,
			                           sc::STUTTER_MODE_COUNT - 1);
		json_t* probJ = json_object_get(rootJ, "probability");
		if (probJ)
			core.probability = clamp((float)json_number_value(probJ), 0.f, 1.f);
	}

	void process(const ProcessArgs& args) override {
		if (modeButton.process(params[MODE_PARAM].getValue() > 0.5f))
			core.mode = (core.mode + 1) % sc::STUTTER_MODE_COUNT;

		// IN1 patched = clock sync: the length knob picks a musical division
		// of the measured period, exactly as on hardware.
		if (inputs[CLOCK_INPUT].isConnected()) {
			clockTimer += args.sampleTime;
			if (clockTrigger.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f)) {
				if (clockTimer < sc::kStutterBufferSec)
					clockPeriod = clockTimer;
				clockTimer = 0.f;
			}
		}
		else {
			clockTimer = 0.f;
			clockPeriod = 0.f;
		}

		const float lenKnob = params[LENGTH_PARAM].getValue();
		if (clockPeriod > 0.f)
			core.sliceSec = clamp(clockPeriod * sc::stutterClockRatio(lenKnob),
			                      0.020f, sc::kStutterBufferSec);
		else
			core.sliceSec = sc::stutterFreeSec(lenKnob);

		core.amount = params[BEHAV_PARAM].getValue();
		core.wet = params[MIX_PARAM].getValue();

		// IN2 stutter gate: repeats while high (>1 V), Schmitt-triggered.
		core.engaged = gateTrigger.process(inputs[GATE_INPUT].getVoltage(), 0.1f, 1.f);

		// +/-5 V -> -1..1 through the shared core and back.
		const float in = inputs[AUDIO_INPUT].getVoltage() / 5.f;
		const float out = core.process(in, args.sampleTime);
		outputs[AUDIO_OUTPUT].setVoltage(clamp(out, -1.f, 1.f) * 5.f);

		// LED flashes on every repeat, as on hardware.
		lights[REPEAT_LIGHT].setBrightness(core.ledLevel());
	}
};

struct StutterWidget : ModuleWidget {
	StutterWidget(Stutter* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/mod2-stutter.svg")));
		// 4 HP Mod1/Mod2 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.03f, 21.7f)), module, Stutter::LENGTH_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 40.06f)), module, Stutter::BEHAV_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 58.42f)), module, Stutter::MIX_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, Stutter::MODE_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, Stutter::REPEAT_LIGHT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, Stutter::CLOCK_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.3f)), module, Stutter::GATE_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, Stutter::AUDIO_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, Stutter::AUDIO_INPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Stutter* module = getModule<Stutter>();
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Auto-stutter probability"));
		// Firmware's BUTTON+POT2 shift layer; a menu slider here.
		menu->addChild(createSubmenuItem("Probability", string::f("%.0f%%", module->core.probability * 100.f),
			[=](Menu* sub) {
				const float vals[] = {1.f, 0.85f, 0.5f, 0.25f, 0.f};
				for (float v : vals)
					sub->addChild(createCheckMenuItem(string::f("%.0f%%", v * 100.f), "",
						[=]() { return module->core.probability == v; },
						[=]() { module->core.probability = v; }));
			}));
	}
};

Model* modelStutter = createModel<Stutter, StutterWidget>("mod2-stutter");
