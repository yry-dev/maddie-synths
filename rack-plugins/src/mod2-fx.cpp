// WIP: Claude-generated maddie synths original (no upstream Hagiwo/Rob
// Scape firmware). Excluded from the VCV Rack build -- see WIP_SOURCES in
// the Makefile and the WIP block in plugin.cpp for how to re-enable it.
#include "plugin.hpp"
#include <FxPlatformCore.h>  // Shared FX platform DSP (also used by mod2-fx firmware)

/*
	FX — multi-algorithm effects platform.

	Port of firmwares/mod2-fx/mod2-fx.ino (HAGIWO Mod2, RP2350).

	One module, sixteen effects behind one identical control surface. The ALGO
	button cycles the algorithm (or pick it from the context menu); every effect
	maps the same two live knobs plus a mix:
		POT1 (A0) -> Main parameter (time / rate / pitch / drive ...)
		POT2 (A1) -> Character (feedback / depth / tone / structure ...)
		POT3 (A2) -> MIX (wet/dry). On hardware POT3's pin is the audio input, so
		             the firmware puts wet/dry on a BUTTON+POT1 shift layer; here
		             the physically-present third knob becomes a proper Mix knob.
		BUTTON    -> short: next algorithm (LED blinks its ID on hardware)
		LED       -> output activity
		IN1       -> clock / tap / trigger (per algorithm)
		IN2       -> gate action (freeze / hold / bypass / damp, per algorithm)
		CV        -> AUDIO INPUT (sampled at the audio rate on hardware)
		OUT       -> audio output

	Only one algorithm runs at a time; all of them share a single int16 arena
	(sized for the engine rate) that the platform hands to whichever core is
	active, and switching mutes ~10 ms while the incoming effect re-inits so it
	never clicks. The per-algorithm long-press "action" (cycle that effect's
	sub-mode) is on the context menu here. The algorithm and every sub-mode
	persist in the patch (firmware: flash).
*/

struct Fx : Module {
	enum ParamId {
		MAIN_PARAM,
		CHAR_PARAM,
		MIX_PARAM,
		ALGO_PARAM,   // momentary button — cycles the algorithm
		PARAMS_LEN
	};
	enum InputId {
		CLOCK_INPUT,  // IN1 — clock / tap / trigger (per algorithm)
		GATE_INPUT,   // IN2 — gate action (per algorithm)
		AUDIO_INPUT,  // CV jack — audio in
		INPUTS_LEN
	};
	enum OutputId {
		AUDIO_OUTPUT, // OUT
		OUTPUTS_LEN
	};
	enum LightId {
		ACTIVITY_LIGHT,
		LIGHTS_LEN
	};

	// The whole FX platform (all 16 cores + the shared arena) lives here.
	sc::FxPlatformCore core;
	std::vector<int16_t> arena;

	dsp::SchmittTrigger clockTrigger;
	dsp::BooleanTrigger algoButton;

	Fx() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(MAIN_PARAM, 0.f, 1.f, 0.5f, "Main");
		configParam(CHAR_PARAM, 0.f, 1.f, 0.5f, "Character");
		configParam(MIX_PARAM, 0.f, 1.f, 0.5f, "Wet/dry mix", "%", 0.f, 100.f);
		configButton(ALGO_PARAM, "Next algorithm");

		configInput(CLOCK_INPUT, "IN1 clock / tap / trigger (per algorithm)");
		configInput(GATE_INPUT, "IN2 gate action (per algorithm)");
		configInput(AUDIO_INPUT, "Audio");
		configOutput(AUDIO_OUTPUT, "Audio");

		configBypass(AUDIO_INPUT, AUDIO_OUTPUT);
		updateArena(APP->engine->getSampleRate());
	}

	// The int16 arena mirrors the firmware's SRAM buffer, sized for the engine
	// rate so every effect's delay / capture memory matches at any sample rate.
	void updateArena(float fs) {
		const uint32_t n = sc::fxArenaSamples(fs);
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
	}

	// The algorithm and every per-algorithm sub-mode persist with the patch
	// (the firmware stores them in flash).
	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "algorithm", json_integer(core.algorithm()));
		json_t* modesJ = json_array();
		for (uint8_t i = 0; i < sc::FX_ALGO_COUNT; i++)
			json_array_append_new(modesJ, json_integer(core.modeOf[i]));
		json_object_set_new(rootJ, "modes", modesJ);
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* modesJ = json_object_get(rootJ, "modes");
		if (modesJ) {
			for (uint8_t i = 0; i < sc::FX_ALGO_COUNT && i < json_array_size(modesJ); i++) {
				json_t* mJ = json_array_get(modesJ, i);
				if (mJ) {
					uint8_t cnt = sc::fxModeCount(i);
					core.modeOf[i] = (uint8_t)clamp((int)json_integer_value(mJ), 0,
					                                cnt ? cnt - 1 : 0);
				}
			}
		}
		json_t* algoJ = json_object_get(rootJ, "algorithm");
		if (algoJ) {
			int a = clamp((int)json_integer_value(algoJ), 0, sc::FX_ALGO_COUNT - 1);
			core.algo = core.pendingAlgo = (uint8_t)a;
			core.swState = sc::FxPlatformCore::SW_NORMAL;
			core.fadeGain = 1.f;
			core.initActive();
		}
	}

	void process(const ProcessArgs& args) override {
		// ALGO button: next algorithm (click-free swap handled by the core).
		if (algoButton.process(params[ALGO_PARAM].getValue() > 0.5f))
			core.nextAlgorithm();

		// Same control surface as the firmware.
		core.setControls(params[MAIN_PARAM].getValue(),
		                 params[CHAR_PARAM].getValue(),
		                 params[MIX_PARAM].getValue());

		// IN1 rising edge -> clock / tap / trigger (per algorithm).
		if (clockTrigger.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f))
			core.onClock();

		// IN2 level -> gate action (per algorithm).
		core.setGate(inputs[GATE_INPUT].getVoltage() > 1.f);

		// Control-rate hook (ring-mod pitch tracker; no-op otherwise).
		core.analyze(args.sampleRate);

		// +/-5 V -> -1..1 through the shared platform and back.
		const float in = inputs[AUDIO_INPUT].getVoltage() / 5.f;
		const float out = core.process(in, args.sampleTime);
		outputs[AUDIO_OUTPUT].setVoltage(clamp(out, -1.f, 1.f) * 5.f);

		// LED = output activity, as on hardware.
		lights[ACTIVITY_LIGHT].setBrightness(core.ledLevel());
	}
};

struct FxWidget : ModuleWidget {
	FxWidget(Fx* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/mod2-fx.svg")));
		// 4 HP Mod1/Mod2 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.03f, 21.7f)), module, Fx::MAIN_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 40.06f)), module, Fx::CHAR_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 58.42f)), module, Fx::MIX_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, Fx::ALGO_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, Fx::ACTIVITY_LIGHT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, Fx::CLOCK_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.3f)), module, Fx::GATE_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, Fx::AUDIO_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, Fx::AUDIO_INPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Fx* module = dynamic_cast<Fx*>(this->module);
		if (!module) return;

		menu->addChild(new MenuSeparator);

		// Algorithm chooser (named entries via the core's name helper).
		menu->addChild(createSubmenuItem("Algorithm", sc::fxAlgoName(module->core.algorithm()),
			[=](Menu* sub) {
				for (uint8_t i = 0; i < sc::FX_ALGO_COUNT; i++) {
					sub->addChild(createCheckMenuItem(sc::fxAlgoName(i), "",
						[=]() { return module->core.algorithm() == i; },
						[=]() { module->core.requestAlgorithm(i); }));
				}
			}));

		// Per-algorithm long-press action = cycle this effect's sub-mode.
		menu->addChild(createMenuItem("Next sub-mode (long-press action)", "",
			[=]() { module->core.action(); }));
	}
};

Model* modelFx = createModel<Fx, FxWidget>("mod2-fx");
