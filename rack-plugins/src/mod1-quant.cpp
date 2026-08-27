#include "plugin.hpp"
#include <QuantCore.h>  // Shared Quant engine (also used by the mod1-quant firmware)

/*
	Quant — note quantizer, 30 scales and chords.

	Port of firmwares/mod1-quant/mod1-quant.ino (HAGIWO MOD1), itself a port of
	Sean Luke's `quant` from the GRAINS collection. The tracker, the scale tables
	and the quantizing are sc::QuantEngine, shared verbatim with the firmware;
	this file owns only the Rack I/O.

	Mirrors the MOD1 hardware: 3 pots, a PWM LED and four jacks.
		POT1  -> TUNE, transpose. Sums into the pitch input, so it moves the
		         quantized note rather than detuning it
		POT2  -> BANK, three banks of ten (modes / other scales / chords)
		POT3  -> SCALE, which of the ten
		LED   -> flashes on every note change
		F1    -> pitch CV in, sums with TUNE
		F3    -> quantized pitch out, 1 V/oct
		F4    -> note-change trigger out, 10 ms

	The hardware's button and F2 are unused, as they are on the firmware: this
	is a quantizer with no modes, and upstream's only compile-time option was
	the output calibration table MOD1 does not need.

	The engine runs its tracker at upstream's 256 Hz control rate off the
	caller's dt, so Rack and the ATmega settle on the same note at the same
	speed, and the input is handed over in ADC counts (0..1023 = 0..5 V) rather
	than volts so the integer filter rounds identically on both. Five octaves in,
	five octaves out: note 0 is 0 V and the tracker tops out at note 59.

	Divergence from the firmware: none in the engine. The one platform-side
	difference is that the pitch output here is a plain voltage instead of the
	firmware's 12-bit PWM, so Rack has no quantization error at all where the
	hardware rounds to within 1.5 cents. The deliberate changes from Sean Luke's
	original are listed in the sketch header.

	License:
	Apache License 2.0, Copyright 2023 Sean Luke (sean@cs.gmu.edu) — the engine
	in sc::QuantEngine is derived from the `quant` firmware in the GRAINS project
	(github.com/eclab/grains), and Apache 2.0 permits modification and
	redistribution only so long as the license notice ships with every copy or
	substantial portion of the work and modified files carry prominent notice of
	their changes. That notice is kept at firmwares/mod1-quant/LICENSE.md; keep
	it there and ship it. Unlike the CC0 HAGIWO modules elsewhere in this plugin,
	this one comes with conditions attached.
	The Rack-side work in this file is MIT License, Copyright (c) 2026 Madelyn
	Yeary, see rack-plugins/LICENSE.md.
*/

// Scale names for the tooltips only — the engine knows these as bitmasks and
// has no business carrying 30 strings onto an AVR. Order matches
// sc::quantScaleMask(), which is upstream's table order.
static const char* kQuantScaleNames[sc::kQuantScaleCount] = {
	"Major", "Harmonic Minor", "Melodic Minor", "Dorian", "Phrygian",
	"Lydian", "Mixolydian", "Aeolian (Relative Minor)", "Locrian", "Chromatic",
	"Blues Minor", "Pentatonic", "Minor Pentatonic", "Japanese Pentatonic",
	"Whole Tone", "Hungarian Gypsy", "Phrygian Dominant", "Persian",
	"Diminished (Octatonic)", "Augmented (Hexatonic)",
	"Octave", "5th + Octave", "Major Triad", "Minor Triad", "Major 6",
	"Minor 6", "7", "Major 7", "Minor 7", "Diminished 7",
};

struct Quant : Module {
	enum ParamId {
		TUNE_PARAM,
		BANK_PARAM,
		SCALE_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		F1_INPUT,     // pitch CV
		INPUTS_LEN
	};
	enum OutputId {
		F3_OUTPUT,    // quantized pitch, 1 V/oct
		F4_OUTPUT,    // note-change trigger
		OUTPUTS_LEN
	};
	enum LightId {
		LED_LIGHT,
		LIGHTS_LEN
	};

	sc::QuantEngine quant;
	dsp::PulseGenerator notePulse;

	Quant();

	void onReset() override { quant.reset(); }

	void process(const ProcessArgs& args) override {
		// Pitch CV sums into TUNE in ADC counts, exactly as the firmware's
		// mod1::addClamp1023(analogRead(POT1), analogRead(CV1)) does: full scale
		// is 5 V, which is the engine's five-octave span.
		const float sum01 = params[TUNE_PARAM].getValue()
		                  + inputs[F1_INPUT].getVoltage() / 5.f;
		const uint16_t adc = (uint16_t)clamp(sum01 * 1023.f, 0.f, 1023.f);

		const uint8_t scale = sc::quantSelectScale(params[BANK_PARAM].getValue() / 2.f,
		                                           params[SCALE_PARAM].getValue());

		if (quant.step(args.sampleTime, adc, scale))
			notePulse.trigger(0.01f);  // 10 ms, the firmware's trigger width

		const bool pulseHigh = notePulse.process(args.sampleTime);

		outputs[F3_OUTPUT].setVoltage(quant.volts());
		outputs[F4_OUTPUT].setVoltage(pulseHigh ? 10.f : 0.f);

		// Same flash level the firmware writes to the LED (200/255).
		lights[LED_LIGHT].setBrightnessSmooth(pulseHigh ? 0.78f : 0.f, args.sampleTime);
	}
};

// SCALE is a plain 0..1 knob whose meaning depends on BANK, so the tooltip asks
// the engine which of the 30 tables the two knobs currently land on.
struct QuantScaleQuantity : ParamQuantity {
	std::string getDisplayValueString() override {
		Quant* m = dynamic_cast<Quant*>(module);
		if (!m)
			return ParamQuantity::getDisplayValueString();
		const uint8_t scale = sc::quantSelectScale(m->params[Quant::BANK_PARAM].getValue() / 2.f,
		                                           getValue());
		return kQuantScaleNames[scale];
	}
};

Quant::Quant() {
	config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

	configParam(TUNE_PARAM, 0.f, 1.f, 0.f, "Tune (transpose)", " semitones", 0.f,
	            (float)(sc::kQuantMaxNote + 1));
	// 0..2 so the snapping switch can reach all three labels; the engine's
	// selector wants 0..1, so call sites divide by 2.
	configSwitch(BANK_PARAM, 0.f, 2.f, 0.f, "Scale bank",
	             {"Modes & chromatic", "Scales", "Chords"});
	configParam<QuantScaleQuantity>(SCALE_PARAM, 0.f, 1.f, 0.f, "Scale");

	configInput(F1_INPUT, "F1 pitch CV");
	configOutput(F3_OUTPUT, "F3 quantized pitch");
	configOutput(F4_OUTPUT, "F4 note-change trigger");

	// The engine is a plain aggregate; the firmware resets it in setup(), so
	// mirror that here — Rack only calls onReset() for an explicit Initialize.
	quant.reset();
}

struct QuantWidget : ModuleWidget {
	QuantWidget(Quant* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/mod1-quant.svg")));

		// 4 HP Mod1 panel — real hole centres (scripts/panels/tools/panel_map.py).
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		// Reversed<> because the Mod1 pots are wired backwards on the panel.
		addParam(createParamCentered<Reversed<RoundBlackKnob>>(mm2px(Vec(10.03f, 21.7f)), module, Quant::TUNE_PARAM));
		addParam(createParamCentered<Reversed<RoundBlackKnob>>(mm2px(Vec(10.04f, 40.06f)), module, Quant::BANK_PARAM));
		addParam(createParamCentered<Reversed<RoundBlackKnob>>(mm2px(Vec(10.04f, 58.42f)), module, Quant::SCALE_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(5.34f, 87.92f)), module, Quant::LED_LIGHT));

		// The button hole and F2 stay empty, as they do on the hardware.
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(5.31f, 99.32f)), module, Quant::F1_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(5.31f, 112.28f)), module, Quant::F3_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(14.71f, 112.28f)), module, Quant::F4_OUTPUT));
	}
};

Model* modelQuant = createModel<Quant, QuantWidget>("mod1-quant");
