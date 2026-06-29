#include "plugin.hpp"
#include <SamplePlayerCore.h>  // Shared PCM playback voice
// Baked break-beat PCM (same data the firmware plays). const arrays have
// internal linkage, so including this in just this TU is self-contained.
#include "../../firmwares/mod2-breakbeats/sample.h"  // sampleDatas[2], sampleLens16[2], startTables[2]

/*
	Breakbeats — PCM break-beat slicer.

	Port of firmwares/mod2-breakbeats/mod2-breakbeats.ino (HAGIWO Mod2, RP2350).

	Plays the two baked drum-loop samples. Each trigger starts playback from one
	of six pre-programmed slice points for a knob-set length; loop mode restarts
	at the end (re-reading the controls, like the firmware's loopRetrig). The
	playback engine (fractional pointer + linear interpolation) is the shared
	sc::SamplePlayer; only slice/length/speed selection lives here.

	The source PCM is 44.1 kHz mono; we advance the read pointer at the source
	rate relative to Rack's engine rate so pitch is sample-rate independent.

	Mod2 hardware mapping:
		POT1 SPEED (0.7-1.5x)   POT2 LENGTH (0.3-2.3 s)   POT3 SLICE (6 points)
		BTN  manual trigger     LED  loop active / playing
		IN1  trigger (rising)   IN2  end-of-playback pulse OUT (5 ms)
		OUT  audio              CV   slice select (summed with POT3)
	Loop on/off and which of the two samples plays are on the right-click menu
	(the hardware multiplexes them onto button press-duration).
*/

static const float BB_SRC_RATE = 44100.0f;  // sample.h PCM rate

struct Breakbeats : Module {
	enum ParamId { SPEED_PARAM, LENGTH_PARAM, SLICE_PARAM, TRIG_PARAM, PARAMS_LEN };
	enum InputId { TRIG_INPUT, SLICE_CV_INPUT, INPUTS_LEN };
	enum OutputId { AUDIO_OUTPUT, EOC_OUTPUT, OUTPUTS_LEN };
	enum LightId { LOOP_LIGHT, LIGHTS_LEN };

	sc::SamplePlayer player;
	dsp::SchmittTrigger gateTrigger;
	dsp::BooleanTrigger buttonTrigger;
	dsp::PulseGenerator eocPulse;

	bool loop = false;       // loop vs one-shot (firmware medium-press)
	int sampleSel = 0;       // 0 or 1 (firmware long-press swap)

	Breakbeats() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(SPEED_PARAM, 0.7f, 1.5f, 1.0f, "Playback speed", "x");
		configParam(LENGTH_PARAM, 0.3f, 2.3f, 1.0f, "Playback length", " s");
		configParam(SLICE_PARAM, 0.f, 1.f, 0.f, "Slice (6 start points)");
		configButton(TRIG_PARAM, "Trigger");
		configInput(TRIG_INPUT, "IN1 trigger");
		configInput(SLICE_CV_INPUT, "Slice select CV");
		configOutput(AUDIO_OUTPUT, "Audio");
		configOutput(EOC_OUTPUT, "End-of-playback pulse");
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "loop", json_boolean(loop));
		json_object_set_new(root, "sampleSel", json_integer(sampleSel));
		return root;
	}
	void dataFromJson(json_t* root) override {
		if (json_t* j = json_object_get(root, "loop")) loop = json_boolean_value(j);
		if (json_t* j = json_object_get(root, "sampleSel")) sampleSel = json_integer_value(j);
	}

	void onReset() override { player.stop(); loop = false; sampleSel = 0; }

	// Start playback from the selected slice (firmware startPlayback()).
	void startPlayback(const ProcessArgs& args) {
		int sel = sampleSel & 1;
		const uint8_t* base = sampleDatas[sel];
		uint32_t total = sampleLens16[sel];
		if (!base || total == 0) return;

		// Slice 0..5 from knob + CV (0..10V).
		float sliceN = params[SLICE_PARAM].getValue()
		             + inputs[SLICE_CV_INPUT].getVoltage() / 10.f;
		int slice = (int)std::round(clamp(sliceN, 0.f, 1.f) * 5.f);
		uint32_t start = (uint32_t)startTables[sel][slice];
		if (start >= total) start = 0;

		// Length in samples, clamped to the sample end.
		uint32_t playLen = (uint32_t)(params[LENGTH_PARAM].getValue() * BB_SRC_RATE);
		if (start + playLen > total) playLen = total - start;

		double step = (double)params[SPEED_PARAM].getValue() * BB_SRC_RATE / args.sampleRate;
		// Core runs one-shot; the module handles loop restarts so each pass
		// re-reads the controls, matching the firmware.
		player.trigger(base + ((size_t)start << 1), playLen, step, false);
	}

	void process(const ProcessArgs& args) override {
		bool fire = buttonTrigger.process(params[TRIG_PARAM].getValue() > 0.5f);
		fire |= gateTrigger.process(inputs[TRIG_INPUT].getVoltage(), 0.1f, 1.f);
		if (fire) startPlayback(args);

		bool ended = false;
		float out = player.process(&ended);
		if (ended) {
			eocPulse.trigger(0.005f);          // 5 ms end-of-playback pulse
			if (loop) startPlayback(args);     // firmware loopRetrig
		}

		outputs[AUDIO_OUTPUT].setVoltage(out * 5.f);  // -1..1 -> +/-5V
		outputs[EOC_OUTPUT].setVoltage(eocPulse.process(args.sampleTime) ? 10.f : 0.f);
		lights[LOOP_LIGHT].setBrightness(loop ? (player.playing ? 1.f : 0.5f)
		                                      : (player.playing ? 0.3f : 0.f));
	}
};

struct BreakbeatsWidget : ModuleWidget {
	BreakbeatsWidget(Breakbeats* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/mod2-breakbeats.svg")));

		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		// 4 HP Mod2 panel — real hole centres (shared with the other mod2 ports).
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.03f, 21.70f)), module, Breakbeats::SPEED_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 40.06f)), module, Breakbeats::LENGTH_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.04f, 58.42f)), module, Breakbeats::SLICE_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(5.19f, 78.57f)), module, Breakbeats::TRIG_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, Breakbeats::LOOP_LIGHT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, Breakbeats::TRIG_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(14.71f, 99.30f)), module, Breakbeats::EOC_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, Breakbeats::AUDIO_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, Breakbeats::SLICE_CV_INPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Breakbeats* m = getModule<Breakbeats>();
		menu->addChild(new MenuSeparator);
		menu->addChild(createBoolPtrMenuItem("Loop", "", &m->loop));
		menu->addChild(createIndexPtrSubmenuItem("Sample", {"Sample 1", "Sample 2"}, &m->sampleSel));
	}
};

Model* modelBreakbeats = createModel<Breakbeats, BreakbeatsWidget>("mod2-breakbeats");
