#include "plugin.hpp"
#include <atomic>
#include <ClkCore.h>  // Shared clock engine (also used by the rabid-audio-clk firmware)

/*
	CLK — "The Count", a 3 HP master clock with a 3-digit 7-segment readout.

	Port of firmwares/rabid-audio-clk/rabid-audio-clk.ino, itself a port of
	rabid.audio's `clock` module (github.com/rabidaudio/synthesizer), used under
	the MIT License, Copyright 2015-2020 Charles Julian Knight. The upstream
	notice ships beside the firmware as firmwares/rabid-audio-clk/SOFTWARE_LICENSE
	and MIT requires it to travel with every copy — unlike the CC0 HAGIWO modules
	in this plugin, which carry no such condition.

	The panel is theirs, mechanics AND artwork: every cutout comes out of their
	`clock.kicad_pcb` at their diameters, and the plate wears their design
	language rather than the maddie synths house style — black ink on bare
	aluminium, a halftone field, tilted slab-typewriter labels
	(scripts/panels/tools/make_clk_panel.py). So this widget's coordinates are the
	real module's: jacks at the TOP, a display that reads vertically, and the
	hardware's modal encoder — not a row of knobs.

	It comes up at 120 BPM with neither latch down, so the encoder is on tempo and
	the readout shows it — the hardware's power-on state.

		BEAT / DIV out  -> 0/10 V, 16 ms gates
		CV in           -> 0..10 V adds 0..255 BPM on top of the dialled tempo
		display         -> three digits stacked, exactly as the hardware reads
		encoder         -> turn: sets whichever parameter is selected
		                   click: tap tempo (the EC11's push-switch)
		DIV / SWING     -> latch one to retarget the encoder; latch BOTH to pause

	Everything above the I/O — tempo, the ratio's two directions, swing, tap
	averaging, the pause latch and the digit formatting — is sc::ClkEngine and
	friends, shared verbatim with the firmware. This file owns only Rack I/O, the
	encoder widget and the display widget.

	Timing translation: the firmware's Timer1 fires an interrupt and asks the
	engine how long until the next tick; here ClkEngine::step() subtracts
	args.sampleTime from the same countdown. Identical tempo at any sample rate,
	and the 16 ms pulse width is the engine's own constant rather than OCR1B.

	How the modal encoder survives the port. On the hardware you HOLD DIV or
	SWING and turn; a mouse cannot hold a button and turn a knob at once. So the
	two buttons LATCH instead of holding — which also makes the DIV+SWING pause
	chord reachable with a mouse, exactly as on the panel. The encoder itself is
	a relative control with no value of its own: it adds detents to whichever of
	the three real parameters is selected. Those parameters stay ordinary
	configParam()s, so presets, patch save/load and the right-click menu all work
	even though only one knob is drawn.

	Two divergences from the hardware, both in PORTING.md:
	  - No save / load / factory-reset gestures. They exist because the hardware
	    has EEPROM and no other way to keep a tempo; Rack persists parameters with
	    the patch, and "Initialize" is the factory reset.
	  - The encoder's push is a click rather than a press-and-hold, so the
	    hold-to-reset gestures layered on it have nowhere to go.
*/

// The reachable subdivision settings, in encoder order. The engine allows -8..4
// and skips -1 and 0 (both would mean "1x"), which is exactly the set the
// hardware's encoder can reach. N >= 1 fires N DIV ticks per beat; N <= -2 fires
// one DIV tick every |N| beats.
static const int8_t kSubdivValues[] = {-8, -7, -6, -5, -4, -3, -2, 1, 2, 3, 4};
static const int kSubdivCount = (int)(sizeof(kSubdivValues) / sizeof(kSubdivValues[0]));
static const int kSubdivDefault = 10;  // -> +4, sc::kClkDefaults.subdivisions

static int subdivIndexOf(int8_t v) {
	for (int i = 0; i < kSubdivCount; i++)
		if (kSubdivValues[i] == v) return i;
	return kSubdivDefault;
}

struct RabidClk : Module {
	enum ParamId {
		// Driven by the encoder, not by knobs of their own (see the note above).
		BPM_PARAM,
		DIV_PARAM,
		SWING_PARAM,
		// The two panel buttons: latches that retarget the encoder, and together
		// form the hardware's pause chord.
		DIVSEL_PARAM,
		SWINGSEL_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		CV_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		BEAT_OUTPUT,
		DIV_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		BEAT_LIGHT,
		DIV_LIGHT,
		LIGHTS_LEN
	};

	// Shared engine + UI helpers, all from ClkCore.h.
	sc::ClkEngine clk;
	sc::ClkTapTempo tapTempo;
	sc::ClkPauseState pauseState;
	// Whichever parameter the encoder last moved owns the display for 1.5 s,
	// the same way the hardware shows the parameter whose button is held.
	sc::ClkChangeTimeout showChange;

	// Set from the UI thread when the encoder is clicked; consumed by process().
	std::atomic<bool> tapRequested{false};
	bool tapHeld = false;      // synthesised gate, so the shared tap core sees an edge
	int tapHoldSamples = 0;
	bool wasPaused = false;

	// What the 7-segment widget draws. LSB-first (digits[0] is the last digit),
	// exactly as the hardware's digit array is ordered.
	char digits[sc::kClkDigits] = {' ', ' ', ' '};

	RabidClk() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

		configParam(BPM_PARAM, (float)sc::kClkMinBPM, (float)sc::kClkMaxBPM,
			(float)sc::kClkDefaults.baseBPM, "Tempo", " BPM");
		paramQuantities[BPM_PARAM]->snapEnabled = true;

		std::vector<std::string> ratios;
		for (int i = 0; i < kSubdivCount; i++) {
			const int8_t n = kSubdivValues[i];
			ratios.push_back(n < 0 ? ("DIV /" + std::to_string(-n))
			                       : ("DIV x" + std::to_string(n)));
		}
		configSwitch(DIV_PARAM, 0.f, (float)(kSubdivCount - 1), (float)kSubdivDefault,
			"BEAT:DIV ratio", ratios);

		configParam(SWING_PARAM, (float)sc::kClkMinSwing, (float)sc::kClkMaxSwing,
			(float)sc::kClkDefaults.swing, "Swing", " %");
		paramQuantities[SWING_PARAM]->snapEnabled = true;

		configSwitch(DIVSEL_PARAM, 0.f, 1.f, 0.f, "Encoder sets DIV",
			{"off", "on"});
		configSwitch(SWINGSEL_PARAM, 0.f, 1.f, 0.f, "Encoder sets SWING",
			{"off", "on"});

		configInput(CV_INPUT, "Tempo CV (0..10V adds 0..255 BPM)");
		configOutput(BEAT_OUTPUT, "Beat");
		configOutput(DIV_OUTPUT, "Division");

		// Come up the way the hardware does: 120 BPM, neither latch down, so the
		// encoder is on tempo and the readout shows it. showChange starts already
		// expired (begin() seeds it past its own timeout) so the first frame shows
		// the live tempo rather than the "mid-edit" base value -- they are the
		// same 120 here, but only because nothing has touched the CV input yet.
		clk.loadSettings(sc::kClkDefaults);
		showChange.begin(1.5f);
	}

	void onReset() override {
		clk.loadSettings(sc::kClkDefaults);
		clk.reset();
		tapTempo.cancel();
		pauseState.reset();
		wasPaused = false;
		tapRequested = false;
		tapHoldSamples = 0;
	}

	// 0 = tempo, 1 = ratio, 2 = swing. Both latches down is pause, and while
	// paused the encoder keeps its last target rather than jumping to tempo.
	int encoderTarget() {
		const bool d = params[DIVSEL_PARAM].getValue() > 0.5f;
		const bool s = params[SWINGSEL_PARAM].getValue() > 0.5f;
		if (d && !s) return 1;
		if (s && !d) return 2;
		return 0;
	}

	// Called from the UI thread by the encoder widget. Routes detents to the
	// selected parameter through the shared engine's own increment rules, so a
	// spin lands on exactly the value the hardware would reach.
	void encoderTurn(int detents) {
		if (detents == 0) return;
		switch (encoderTarget()) {
			case 1: {
				const int8_t v = clk.incrementSubdivisions(detents);
				params[DIV_PARAM].setValue((float)subdivIndexOf(v));
				break;
			}
			case 2: {
				// Swing moves three points per detent, as on the hardware.
				const int8_t v = clk.incrementSwing((int16_t)(3 * detents));
				params[SWING_PARAM].setValue((float)v);
				break;
			}
			default: {
				const uint16_t v = clk.incrementBaseBPM((int16_t)detents);
				params[BPM_PARAM].setValue((float)v);
				break;
			}
		}
		showChange.noteChanged();
	}

	void encoderPush() { tapRequested = true; }

	void process(const ProcessArgs& args) override {
		const float dt = args.sampleTime;

		// ── Parameters -> engine. The setters no-op when unchanged, so the
		// derived timing is recomputed only on a real edit. Reading them every
		// sample (rather than only on encoder turns) keeps the engine in step
		// with preset loads, undo and the right-click menu.
		const int divIdx = clamp((int)std::round(params[DIV_PARAM].getValue()),
		                         0, kSubdivCount - 1);
		clk.setSubdivisions(kSubdivValues[divIdx]);
		clk.setSwing((int8_t)std::round(params[SWING_PARAM].getValue()));
		clk.setBaseBPM((uint16_t)std::round(params[BPM_PARAM].getValue()));

		// CV: the firmware reads a 10-bit ADC and divides by 4, giving 0..255 BPM
		// on top of the dialled tempo. 0..10 V maps onto the same range.
		const float cv = clamp(inputs[CV_INPUT].getVoltage(), 0.f, 10.f);
		clk.setBPMOffset((uint16_t)(cv * (1023.f / 10.f) / 4.f));

		// ── Encoder click -> a short gate, so the shared tap core sees the same
		// press/release edge pair the hardware button produces.
		if (tapRequested.exchange(false)) tapHoldSamples = (int)(args.sampleRate * 0.02f);
		if (tapHoldSamples > 0) {
			tapHoldSamples--;
			tapHeld = true;
		}
		else {
			tapHeld = false;
		}

		// ── Pause: the DIV+SWING chord, latched.
		const bool chord = params[DIVSEL_PARAM].getValue() > 0.5f
		                && params[SWINGSEL_PARAM].getValue() > 0.5f;
		pauseState.setState(chord);
		// Kept fresh every sample so entering pause never inherits a stale edge.
		const bool tapEdge = pauseState.isTap(tapHeld);
		const bool paused = pauseState.isPaused();

		if (paused != wasPaused) {
			if (paused) {
				// Freeze at the start of the bar so the next run begins on a
				// downbeat, as the firmware does by resetting every loop.
				clk.reset();
				tapTempo.cancel();
			}
			wasPaused = paused;
		}
		clk.setEnabled(!paused);
		if (paused && tapEdge) {
			// Rewind first, so a single step always fires BEAT and DIV together.
			// That is the gesture's whole purpose ("restart a sequencer on its
			// downbeat"), and it is what the firmware gets for free by calling
			// reset() on every loop it spends paused.
			clk.reset();
			clk.singleTick();
		}

		// Tap tempo has to see every sample, not just the ones where TAP is down.
		const uint16_t tapBpm = tapTempo.tick(dt, tapHeld);
		if (!paused && tapTempo.isActive() && tapBpm >= sc::kClkMinBPM) {
			// Drive the parameter itself, so a tapped tempo survives a patch save.
			params[BPM_PARAM].setValue((float)std::min(tapBpm, sc::kClkMaxBPM));
			showChange.noteChanged();
		}

		// ── Run the clock ──────────────────────────────────────────────────
		clk.step(dt);

		outputs[BEAT_OUTPUT].setVoltage(clk.beatOut() ? 10.f : 0.f);
		outputs[DIV_OUTPUT].setVoltage(clk.divOut() ? 10.f : 0.f);
		lights[BEAT_LIGHT].setBrightnessSmooth(clk.beatOut() ? 1.f : 0.f, dt);
		lights[DIV_LIGHT].setBrightnessSmooth(clk.divOut() ? 1.f : 0.f, dt);

		// ── Display ────────────────────────────────────────────────────────
		showChange.tick(dt);
		const int target = encoderTarget();
		if (paused) {
			sc::clkFormatPaused(digits);
		}
		else if (target == 2) {
			sc::clkFormatNumber(digits, (int16_t)clk.swing);
		}
		else if (target == 1) {
			sc::clkFormatSubdiv(digits, clk.subdivisions);
		}
		else if (showChange.isChanging()) {
			// Mid-edit: the tempo being dialled in, without the CV offset.
			sc::clkFormatNumber(digits, (int16_t)clk.baseBPM);
		}
		else {
			// Settled: the rate actually coming out of the BEAT jack.
			sc::clkFormatNumber(digits, (int16_t)clk.bpm());
		}
	}
};

// ── 7-segment readout ──────────────────────────────────────────────────────
//
// Three digits STACKED VERTICALLY, because that is how the hardware fits a
// 3-digit driver behind a 3 HP face: the SM460281N digits sit on a 10.795 mm
// pitch behind an 8 x 32.1 mm window, so "120" reads down the panel. Glyphs come
// from the shared font (sc::clkSegments), so this shows exactly what the
// hardware's LEDs would. Unlit segments stay faintly visible, like a real LED.
struct SevenSegWidget : TransparentWidget {
	RabidClk* module = nullptr;

	static constexpr float kGlassPad = 1.5f;   // px inset from the window
	static constexpr float kDigitGap = 2.5f;   // px between stacked digits
	static constexpr float kThickFrac = 0.15f; // segment thickness / digit height

	// Filled hexagon for one segment: a bar of length `len` and thickness `t`,
	// centred at (cx, cy), mitred at both ends so neighbouring segments meet.
	static void bar(NVGcontext* vg, float cx, float cy, float len, float t, bool vertical) {
		const float h = len / 2.f, q = t / 2.f;
		nvgBeginPath(vg);
		if (vertical) {
			nvgMoveTo(vg, cx, cy - h);
			nvgLineTo(vg, cx + q, cy - h + q);
			nvgLineTo(vg, cx + q, cy + h - q);
			nvgLineTo(vg, cx, cy + h);
			nvgLineTo(vg, cx - q, cy + h - q);
			nvgLineTo(vg, cx - q, cy - h + q);
		}
		else {
			nvgMoveTo(vg, cx - h, cy);
			nvgLineTo(vg, cx - h + q, cy - q);
			nvgLineTo(vg, cx + h - q, cy - q);
			nvgLineTo(vg, cx + h, cy);
			nvgLineTo(vg, cx + h - q, cy + q);
			nvgLineTo(vg, cx - h + q, cy + q);
		}
		nvgClosePath(vg);
		nvgFill(vg);
	}

	void drawDigit(NVGcontext* vg, float x, float y, float w, float h, uint8_t segs) {
		const NVGcolor on = nvgRGB(0xff, 0x4a, 0x2c);   // amber-red LED
		const NVGcolor off = nvgRGBA(0xff, 0x4a, 0x2c, 0x1c);
		const float t = h * kThickFrac;
		const float gap = t * 0.30f;
		const float hLen = w - t;
		const float vLen = (h - t) / 2.f - gap;
		const float xl = x + t / 2.f, xr = x + w - t / 2.f, xc = x + w / 2.f;
		const float yt = y + t / 2.f, ym = y + h / 2.f, yb = y + h - t / 2.f;

		struct Seg { uint8_t bit; float cx, cy, len; bool vertical; };
		const Seg segments[7] = {
			{sc::kSegA, xc, yt, hLen, false},
			{sc::kSegB, xr, (yt + ym) / 2.f, vLen, true},
			{sc::kSegC, xr, (ym + yb) / 2.f, vLen, true},
			{sc::kSegD, xc, yb, hLen, false},
			{sc::kSegE, xl, (ym + yb) / 2.f, vLen, true},
			{sc::kSegF, xl, (yt + ym) / 2.f, vLen, true},
			{sc::kSegG, xc, ym, hLen, false},
		};
		for (const Seg& s : segments) {
			nvgFillColor(vg, (segs & s.bit) ? on : off);
			bar(vg, s.cx, s.cy, s.len, t, s.vertical);
		}
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		// Layer 1 is Rack's self-illuminated pass, so the readout stays lit when
		// the rack lights are dimmed — which is how a real LED display behaves.
		if (layer != 1) {
			TransparentWidget::drawLayer(args, layer);
			return;
		}
		nvgBeginPath(args.vg);
		nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
		nvgFillColor(args.vg, nvgRGB(0x10, 0x08, 0x0a));
		nvgFill(args.vg);

		const char* fallback = "021";  // LSB-first "120", for the module browser
		const float w = box.size.x - 2 * kGlassPad;
		const float h = (box.size.y - 2 * kGlassPad - 2 * kDigitGap) / sc::kClkDigits;
		for (int i = 0; i < (int)sc::kClkDigits; i++) {
			// digits[0] is the least significant, and the stack reads downward,
			// so the most significant digit is drawn at the top.
			const char c = module ? module->digits[i] : fallback[i];
			const float y = kGlassPad + (float)(sc::kClkDigits - 1 - i) * (h + kDigitGap);
			drawDigit(args.vg, kGlassPad, y, w, h, sc::clkSegments(c));
		}
		TransparentWidget::drawLayer(args, layer);
	}
};

// ── The encoder ────────────────────────────────────────────────────────────
//
// An EC11 with a push-switch: endless, so it has no position to show and no
// parameter of its own. Dragging feeds detents to whichever parameter the two
// latches select; a click without a drag is the push-switch, i.e. tap tempo.
struct EncoderWidget : Widget {
	RabidClk* module = nullptr;
	float accum = 0.f;      // sub-detent travel
	float travel = 0.f;     // total drag distance, to tell a click from a turn
	float spin = 0.f;       // visual cap rotation, radians

	static constexpr float kPxPerDetent = 6.f;
	static constexpr float kClickPx = 2.f;

	void onButton(const event::Button& e) override {
		if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
			e.consume(this);   // claim the drag
			return;
		}
		Widget::onButton(e);
	}

	void onDragStart(const event::DragStart& e) override {
		accum = 0.f;
		travel = 0.f;
		APP->window->cursorLock();
	}

	void onDragMove(const event::DragMove& e) override {
		if (!module) return;
		// Up and to the right both read as clockwise, matching Rack's knobs.
		const float d = (e.mouseDelta.y * -1.f + e.mouseDelta.x) * 0.5f;
		travel += std::fabs(e.mouseDelta.x) + std::fabs(e.mouseDelta.y);
		accum += d;
		// Fine mode: hold shift for one detent per full sweep of the coarse step.
		const float perDetent = ((APP->window->getMods() & RACK_MOD_MASK) == GLFW_MOD_SHIFT)
			? kPxPerDetent * 4.f : kPxPerDetent;
		int detents = (int)(accum / perDetent);
		if (detents != 0) {
			accum -= detents * perDetent;
			module->encoderTurn(detents);
			spin += detents * 0.35f;
		}
	}

	void onDragEnd(const event::DragEnd& e) override {
		APP->window->cursorUnlock();
		// A press that never really moved is the shaft's push-switch.
		if (module && travel < kClickPx) module->encoderPush();
	}

	void draw(const DrawArgs& args) override {
		const float r = box.size.x / 2.f;
		const Vec c = box.size.div(2.f);
		// Cap body
		nvgBeginPath(args.vg);
		nvgCircle(args.vg, c.x, c.y, r);
		nvgFillPaint(args.vg, nvgLinearGradient(args.vg, 0, 0, 0, box.size.y,
			nvgRGB(0x3a, 0x3a, 0x3e), nvgRGB(0x18, 0x18, 0x1b)));
		nvgFill(args.vg);
		nvgStrokeColor(args.vg, nvgRGB(0x50, 0x50, 0x56));
		nvgStrokeWidth(args.vg, 1.f);
		nvgStroke(args.vg);
		// Knurling — turns with the encoder, the only motion feedback it can give
		nvgStrokeColor(args.vg, nvgRGBA(0xe8, 0xe8, 0xf0, 0x50));
		nvgStrokeWidth(args.vg, 1.2f);
		for (int i = 0; i < 16; i++) {
			const float a = spin + (float)i * (2.f * M_PI / 16.f);
			nvgBeginPath(args.vg);
			nvgMoveTo(args.vg, c.x + std::cos(a) * r * 0.74f, c.y + std::sin(a) * r * 0.74f);
			nvgLineTo(args.vg, c.x + std::cos(a) * r * 0.94f, c.y + std::sin(a) * r * 0.94f);
			nvgStroke(args.vg);
		}
		// Recessed centre, where the push-switch lives
		nvgBeginPath(args.vg);
		nvgCircle(args.vg, c.x, c.y, r * 0.55f);
		nvgFillColor(args.vg, nvgRGB(0x24, 0x24, 0x28));
		nvgFill(args.vg);
		Widget::draw(args);
	}
};

struct RabidClkWidget : ModuleWidget {
	RabidClkWidget(RabidClk* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/rabid-audio-clk.svg")));

		// 3 HP panel (15.2 mm). Every coordinate below is rabid.audio's own,
		// read out of clock/clock/clock.kicad_pcb — see
		// scripts/panels/tools/rabid-audio-clk-panel.json.
		addChild(createWidget<ScrewSilver>(mm2px(Vec(7.5f, 3.0f))
			.minus(Vec(RACK_GRID_WIDTH / 2, RACK_GRID_WIDTH / 2))));
		addChild(createWidget<ScrewSilver>(mm2px(Vec(7.5f, 125.5f))
			.minus(Vec(RACK_GRID_WIDTH / 2, RACK_GRID_WIDTH / 2))));

		// Jacks at the top — their layout, the inverse of the mod1/mod2 grid.
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(7.25f, 14.40f)), module, RabidClk::BEAT_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(7.25f, 24.50f)), module, RabidClk::DIV_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(7.25f, 34.65f)), module, RabidClk::CV_INPUT));

		addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(3.875f, 42.39f)), module, RabidClk::BEAT_LIGHT));
		addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(11.900f, 42.39f)), module, RabidClk::DIV_LIGHT));

		// The tall window: three digits stacked, reading downward.
		SevenSegWidget* seg = new SevenSegWidget;
		seg->module = module;
		seg->box.pos = mm2px(Vec(3.875f, 45.405f));
		seg->box.size = mm2px(Vec(8.025f, 32.090f));
		addChild(seg);

		// EC11 encoder: turn to set the selected parameter, click to tap.
		EncoderWidget* enc = new EncoderWidget;
		enc->module = module;
		enc->box.size = mm2px(Vec(10.0f, 10.0f));
		enc->box.pos = mm2px(Vec(7.625f, 91.60f)).minus(enc->box.size.div(2.f));
		addChild(enc);

		// The two latches. Either one alone retargets the encoder; both together
		// are the hardware's pause chord.
		addParam(createParamCentered<VCVLatch>(mm2px(Vec(5.565f, 104.341f)), module, RabidClk::DIVSEL_PARAM));
		addParam(createParamCentered<VCVLatch>(mm2px(Vec(5.565f, 115.136f)), module, RabidClk::SWINGSEL_PARAM));
	}
};

Model* modelRabidClk = createModel<RabidClk, RabidClkWidget>("rabid-audio-clk");
