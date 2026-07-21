#pragma once

// FX Platform — multi-algorithm effects host (the "MOD2 FX" engine).
//
// Used by:
//   - firmwares/mod2-fx/mod2-fx.ino
//   - rack-plugins/src/mod2-fx.cpp
//
// One firmware, many effects: the MOD2 answer to an FX Aid / Pico DSP. This is
// the shared glue that BOTH the firmware and the VCV Rack port call. It hosts a
// bank of the existing pure-C++ sc:: effect cores behind a single, identical
// control surface and lets the platform switch between them click-free:
//
//   - Two normalised live controls: main (POT1) and character (POT2). Each
//     algorithm's adapter maps these onto whatever that core's own params are
//     (they are NOT uniform — the adapter is per-algorithm, see applyParams()).
//   - A wet/dry mix (BUTTON+POT1 shift layer on hardware, POT3/MIX in Rack).
//   - IN1 = clock / tap / trigger, meaning per algorithm (tap-tempo for the
//     time-based effects; strike / pluck / retrigger / recapture for the rest).
//   - IN2 = gate action, meaning per algorithm (hold / freeze / reverse-ish /
//     damp / bypass — see setGate()).
//   - A per-algorithm long-press "action" that cycles that effect's own
//     sub-mode (Clean/Dirty, Hall/Plate, the distortion flavours, ...).
//
// Memory strategy — ONE shared arena, reused by whichever algorithm runs (only
// one runs at a time). All 16 cores are held as plain members; their *state*
// structs are tiny (delay-line pointers, filter states, a couple of KB total),
// and the big delay/capture memory is the single caller-provided int16 arena
// that init() hands to the active core. On an algorithm change the incoming
// core is fully re-initialised (which clears the shared arena) during a short
// output mute so switching never clicks or reads another core's leftover
// buffer. This "all-members + one arena" layout was chosen over a union because
// the per-core state is small enough that the union saves little, keeps the
// code trivially safe (no manual lifetime management / placement-new), and the
// firmware still fits RP2350 RAM once the arena is bounded (see kFxArenaSec).
//
// Nothing is excluded: all 16 planned algorithms are hosted (v1 essentials,
// v2 performance, v3 ambient, plus karplus / comb / pitch-shift). If a future
// build stops fitting flash/RAM, drop cores from the tail of FxAlgo (which is
// ordered by the README's roadmap priority) — the enum, name table and adapter
// are the only things to trim.
//
// Pure C++: depends only on <math.h>/<stdint.h> via sc_math.h / sc_dsp.h and
// the (equally pure) sc effect cores. No Arduino.h, rack.hpp or Pico SDK;
// float only, no heap, no STL — compiles on AVR, RP2350 and the desktop.

#include <math.h>
#include <stdint.h>

#include "sc_math.h"
#include "sc_dsp.h"

// Hosted effect cores (all pure, header-only, namespace sc).
#include "DelayFxCore.h"
#include "DistortionCore.h"
#include "BitcrusherCore.h"
#include "ChorusCore.h"
#include "ResonatorCore.h"
#include "TapeEchoCore.h"
#include "FlangerCore.h"
#include "PhaserCore.h"
#include "RingModCore.h"
#include "WavefolderCore.h"
#include "ReverbCore.h"
#include "FreezeCore.h"
#include "GlitchDelayCore.h"
#include "KarplusCore.h"
#include "CombCore.h"
#include "PitchShifterCore.h"

namespace sc {

// Algorithm table — ordered by the README roadmap priority (v1 first). The
// index+1 is the LED blink code on hardware. Keep additions at the tail so the
// blink codes / saved-preset indices stay stable.
enum FxAlgo : uint8_t {
  FX_DELAY = 0,     // v1
  FX_DISTORTION,    // v1
  FX_BITCRUSHER,    // v1
  FX_CHORUS,        // v1
  FX_RESONATOR,     // v1
  FX_TAPE,          // v2
  FX_FLANGER,       // v2
  FX_PHASER,        // v2
  FX_RINGMOD,       // v2
  FX_WAVEFOLDER,    // v2
  FX_REVERB,        // v3
  FX_FREEZE,        // v3
  FX_GLITCH,        // v3
  FX_KARPLUS,       // extra
  FX_COMB,          // extra
  FX_PITCH,         // extra
  FX_ALGO_COUNT
};

// --------------------------------------------------------------------------
// Shared arena sizing. One int16 buffer is reused by whichever algorithm runs.
// 2.5 s keeps the firmware comfortably inside RP2350 SRAM (~183 KB at the
// 36.6 kHz audio rate) while still giving a long delay / freeze / glitch
// buffer. The Rack port sizes the same number of *seconds* at the engine rate
// so the effects sound the same at any host sample rate.
// --------------------------------------------------------------------------
constexpr float kFxArenaSec = 2.5f;

inline uint32_t fxArenaSamples(float fsHz) {
  return (uint32_t)(kFxArenaSec * fsHz) + 64;
}

// Number of sub-modes each algorithm cycles through on the long-press action.
inline uint8_t fxModeCount(uint8_t algo) {
  switch (algo) {
    case FX_DELAY:       return DELAYFX_MODE_COUNT;      // 2
    case FX_DISTORTION:  return DISTORTION_MODE_COUNT;   // 5
    case FX_BITCRUSHER:  return BITCRUSH_MODE_COUNT;     // 3
    case FX_CHORUS:      return CHORUS_MODE_COUNT;       // 3
    case FX_RESONATOR:   return RESONATOR_MODE_COUNT;    // 3
    case FX_TAPE:        return 1;                       // no sub-mode
    case FX_FLANGER:     return FLANGER_SHAPE_COUNT;     // 3
    case FX_PHASER:      return PHASER_STAGES_COUNT;     // 3
    case FX_RINGMOD:     return RINGMOD_MODE_COUNT;      // 3
    case FX_WAVEFOLDER:  return WAVEFOLDER_MODE_COUNT;   // 3
    case FX_REVERB:      return REVERB_MODE_COUNT;       // 2
    case FX_FREEZE:      return FREEZE_MODE_COUNT;       // 3
    case FX_GLITCH:      return GLITCH_PALETTE_COUNT;    // 4
    case FX_KARPLUS:     return KARPLUS_MODE_COUNT;      // 3
    case FX_COMB:        return COMB_MODE_COUNT;         // 3
    case FX_PITCH:       return PITCH_MODE_COUNT;        // 4
    default:             return 1;
  }
}

// Short display name for each algorithm (context menu / docs).
inline const char* fxAlgoName(uint8_t algo) {
  switch (algo) {
    case FX_DELAY:       return "Delay";
    case FX_DISTORTION:  return "Distortion";
    case FX_BITCRUSHER:  return "Bitcrusher";
    case FX_CHORUS:      return "Chorus";
    case FX_RESONATOR:   return "Resonator";
    case FX_TAPE:        return "Tape Echo";
    case FX_FLANGER:     return "Flanger";
    case FX_PHASER:      return "Phaser";
    case FX_RINGMOD:     return "Ring Mod";
    case FX_WAVEFOLDER:  return "Wavefolder";
    case FX_REVERB:      return "Reverb";
    case FX_FREEZE:      return "Freeze";
    case FX_GLITCH:      return "Glitch Delay";
    case FX_KARPLUS:     return "Karplus";
    case FX_COMB:        return "Comb";
    case FX_PITCH:       return "Pitch Shift";
    default:             return "?";
  }
}

// True when IN2 (the gate) should act as a plain wet-defeat bypass for this
// algorithm — i.e. the core has no more specific gate action of its own.
inline bool fxGateIsBypass(uint8_t algo) {
  switch (algo) {
    case FX_DISTORTION:
    case FX_BITCRUSHER:
    case FX_CHORUS:
    case FX_FLANGER:
    case FX_PHASER:
    case FX_WAVEFOLDER:
    case FX_PITCH:
      return true;
    default:
      return false;  // delay/tape/reverb/freeze/comb/karplus/resonator/ringmod/glitch
  }
}

struct FxPlatformCore {
  // ---- hosted cores (all state lives here; big buffers share the arena) ----
  DelayFxCore      delay;
  DistortionCore   distortion;
  BitcrusherCore   bitcrusher;
  ChorusCore       chorus;
  ResonatorCore    resonator;
  TapeEchoCore     tape;
  FlangerCore      flanger;
  PhaserCore       phaser;
  RingModCore      ringmod;
  WavefolderCore   wavefolder;
  ReverbCore       reverb;
  FreezeCore       freezeC;
  GlitchDelayCore  glitch;
  KarplusCore      karplus;
  CombCore         comb;
  PitchShifterCore pitch;

  // ---- shared arena ----
  int16_t* arena = nullptr;
  uint32_t arenaLen = 0;

  // ---- control state (set from the platform; adapter maps to the core) ----
  float pot1 = 0.5f;   // main
  float pot2 = 0.5f;   // character
  float wet = 0.5f;    // wet/dry mix
  bool  gateHi = false;  // IN2 level
  uint8_t modeOf[FX_ALGO_COUNT] = {0};  // per-algorithm sub-mode (persists)

  // ---- algorithm / click-free switch state ----
  uint8_t algo = FX_DELAY;
  uint8_t pendingAlgo = FX_DELAY;
  enum Switch : uint8_t { SW_NORMAL = 0, SW_FADE_OUT = 1, SW_FADE_IN = 2 };
  uint8_t swState = SW_NORMAL;
  float fadeGain = 1.0f;
  static constexpr float kFadeSec = 0.010f;  // ~10 ms mute each side of a swap

  // ---- clock / tap-tempo (for the time-based algorithms) ----
  float clockTimer = 0.0f;      // seconds since the last IN1 edge
  float clockPeriod = 0.0f;     // measured period (0 = none / timed out)
  bool  clockValid = false;

  // ---- per-algorithm one-shot events (consumed in process) ----
  bool bitExtTick = false;      // reserved (unused: bitcrusher uses internal rate)

  // ---- LED activity follower (uniform across every algorithm) ----
  float ledEnv = 0.0f;

  // -----------------------------------------------------------------------
  // Lifecycle
  // -----------------------------------------------------------------------
  // `buf`/`n`: the one caller-owned int16 arena, fxArenaSamples() long.
  void init(int16_t* buf, uint32_t n) {
    arena = buf;
    arenaLen = n;
    swState = SW_NORMAL;
    fadeGain = 1.0f;
    clockTimer = clockPeriod = 0.0f;
    clockValid = false;
    ledEnv = 0.0f;
    initActive();  // give the arena to the current algorithm
  }

  void reset() {
    for (uint8_t i = 0; i < FX_ALGO_COUNT; i++) modeOf[i] = 0;
    algo = pendingAlgo = FX_DELAY;
    swState = SW_NORMAL;
    fadeGain = 1.0f;
    clockTimer = clockPeriod = 0.0f;
    clockValid = false;
    ledEnv = 0.0f;
    initActive();
  }

  uint8_t algorithm() const { return algo; }
  uint8_t currentMode() const { return modeOf[algo]; }
  static uint8_t algorithmCount() { return FX_ALGO_COUNT; }
  static const char* algorithmName(uint8_t i) { return fxAlgoName(i); }

  // -----------------------------------------------------------------------
  // Control inputs (the identical surface every algorithm shares)
  // -----------------------------------------------------------------------
  void setControls(float main01, float character01, float wet01) {
    pot1 = clampf(main01, 0.0f, 1.0f);
    pot2 = clampf(character01, 0.0f, 1.0f);
    wet = clampf(wet01, 0.0f, 1.0f);
  }
  void setMain(float v) { pot1 = clampf(v, 0.0f, 1.0f); }
  void setCharacter(float v) { pot2 = clampf(v, 0.0f, 1.0f); }
  void setWet(float v) { wet = clampf(v, 0.0f, 1.0f); }

  // IN2 gate level.
  void setGate(bool hi) { gateHi = hi; }

  // BUTTON short-press (or Rack ALGO button): advance to the next algorithm,
  // click-free. Safe to call from a control-rate context; the actual swap
  // (arena clear + core re-init) happens inside process() while muted.
  void nextAlgorithm() { requestAlgorithm((algo + 1) % FX_ALGO_COUNT); }

  void requestAlgorithm(uint8_t a) {
    if (a >= FX_ALGO_COUNT) return;
    if (a == algo && swState == SW_NORMAL) return;
    pendingAlgo = a;
    swState = SW_FADE_OUT;  // fade out -> swap at silence -> fade in
  }

  // BUTTON long-press: the per-algorithm action = cycle this effect's sub-mode.
  void action() {
    const uint8_t n = fxModeCount(algo);
    if (n <= 1) return;
    modeOf[algo] = (uint8_t)((modeOf[algo] + 1) % n);
    if (algo == FX_WAVEFOLDER)   // wavefolder wants a crossfade on mode change
      wavefolder.setMode(modeOf[FX_WAVEFOLDER]);
  }
  void setMode(uint8_t m) {
    const uint8_t n = fxModeCount(algo);
    modeOf[algo] = n ? (uint8_t)(m % n) : 0;
    if (algo == FX_WAVEFOLDER) wavefolder.setMode(modeOf[FX_WAVEFOLDER]);
  }

  // IN1 rising edge — meaning depends on the algorithm.
  void onClock() {
    switch (algo) {
      case FX_DELAY:
      case FX_TAPE:
      case FX_GLITCH: {
        // Tap tempo / clock: adopt the measured interval as the period.
        const float p = clockTimer;
        if (p >= 0.030f && p <= kFxArenaSec) {
          clockPeriod = p;
          clockValid = true;
        }
        clockTimer = 0.0f;
        break;
      }
      case FX_RESONATOR: resonator.strike(); break;
      case FX_KARPLUS:   karplus.pluck();    break;
      case FX_RINGMOD:   ringmod.trigger();  break;
      case FX_FLANGER:   flanger.retrigger(); break;
      case FX_PHASER:    phaser.retrigger();  break;
      case FX_FREEZE:    freezeC.recapture(); break;
      default: break;  // distortion / bitcrusher / chorus / wavefolder /
                       // reverb / comb / pitch: IN1 unused
    }
  }

  // Control-rate hook (call from loop() in firmware, once per Rack block is
  // fine). Only the pitch-tracking ring-mod actually does work here.
  void analyze(float fsAudio) {
    if (algo == FX_RINGMOD) ringmod.analyzePitch(fsAudio);
  }

  // 0..1 LED brightness — a uniform output-activity follower. The firmware
  // overrides this to blink the algorithm ID after a switch.
  float ledLevel() const { return clampf(ledEnv * 2.0f, 0.0f, 1.0f); }

  // -----------------------------------------------------------------------
  // Internal: (re-)initialise the active core, handing it the shared arena.
  // Cores with delay/capture memory take the arena; memoryless cores just
  // reset. This clears the arena, so it runs only while output is muted.
  // -----------------------------------------------------------------------
  void initActive() {
    switch (algo) {
      case FX_DELAY:       delay.init(arena, arenaLen); break;
      case FX_CHORUS:      chorus.init(arena, arenaLen); break;
      case FX_RESONATOR:   resonator.init(arena, arenaLen); break;
      case FX_TAPE:        tape.init(arena, arenaLen); break;
      case FX_FLANGER:     flanger.init(arena, arenaLen); break;
      case FX_REVERB:      reverb.init(arena, arenaLen); break;
      case FX_FREEZE:      freezeC.init(arena, arenaLen); break;
      case FX_GLITCH:      glitch.init(arena, arenaLen); break;
      case FX_KARPLUS:     karplus.init(arena, arenaLen); break;
      case FX_COMB:        comb.init(arena, arenaLen); break;
      case FX_PITCH:       pitch.init(arena, arenaLen); break;
      // Memoryless cores: no arena, just power-on state.
      case FX_DISTORTION:  distortion.reset(); break;
      case FX_BITCRUSHER:  bitcrusher.reset(); break;
      case FX_PHASER:      phaser.reset(); break;
      case FX_RINGMOD:     ringmod.reset(); break;
      case FX_WAVEFOLDER:  wavefolder.reset(); wavefolder.setMode(modeOf[FX_WAVEFOLDER]); break;
      default: break;
    }
  }

  // Map the two normalised controls + wet + gate onto the active core's own
  // parameters. Called every sample (cheap) so a live knob / clock is tracked.
  void applyParams(float dt) {
    const float fs = 1.0f / dt;
    // Arena length in seconds, with a little margin so time reads never hit the
    // very end of the buffer.
    const float arenaSec = (float)arenaLen * dt;
    const float maxSec = arenaSec * 0.92f;
    const uint8_t m = modeOf[algo];

    switch (algo) {
      case FX_DELAY:
        delay.timeSec = clockValid
                          ? clampf(clockPeriod * delayFxClockRatio(pot1), 0.010f, maxSec)
                          : delayFxTimeSec(pot1, maxSec);
        delay.feedback = delayFxFeedback(pot2);
        delay.mode = m;
        delay.wet = wet;
        delay.hold = gateHi;  // IN2 = hold (infinite repeat)
        break;

      case FX_DISTORTION:
        distortion.drive = distortionDriveGain(pot1);
        distortion.tone = pot2;
        distortion.mode = m;
        distortion.wet = wet;
        break;

      case FX_BITCRUSHER:
        bitcrusher.rateHz = bitcrusherRateHz(pot1, fs);
        bitcrusher.bits = bitcrusherBits(pot2);
        bitcrusher.mode = m;
        bitcrusher.wet = wet;
        break;

      case FX_CHORUS:
        chorus.rateHz = chorusRateHz(pot1);
        chorus.depth = pot2;
        chorus.mode = m;
        chorus.wet = wet;
        break;

      case FX_RESONATOR:
        resonator.pitchHz = resonatorPitchHz(pot1);
        resonator.structure = pot2;   // character = timbre spread
        resonator.damping = 0.55f;    // fixed musical decay (only 2 live knobs)
        resonator.mode = m;
        resonator.wet = wet;
        resonator.dampGate = gateHi;  // IN2 = damp/choke
        break;

      case FX_TAPE:
        tape.timeSec = clockValid
                         ? clampf(clockPeriod * delayFxClockRatio(pot1), 0.010f, maxSec)
                         : tapeEchoTimeSec(pot1, maxSec);
        tape.age = pot2;
        tape.feedback = 0.30f + 0.60f * pot2;  // worn tape -> more repeats
        tape.wet = wet;
        tape.splice = gateHi;  // IN2 = tape-stop splice
        break;

      case FX_FLANGER:
        flanger.manual = flangerManual(pot1);
        flanger.manualPos = pot1;
        flanger.rateHz = flangerRateHz(pot1);
        flanger.feedback = flangerFeedback(pot2);
        flanger.depth = 0.7f;
        flanger.shape = m;
        flanger.wet = wet;
        break;

      case FX_PHASER:
        phaser.manual = phaserManual(pot1);
        phaser.manualPos = pot1;
        phaser.rateHz = phaserRateHz(pot1);
        phaser.feedback = phaserFeedback(pot2);
        phaser.depth = 0.8f;
        phaser.stageSel = m;
        phaser.wet = wet;
        break;

      case FX_RINGMOD:
        if (m == RINGMOD_TRACK) ringmod.trackRatio = ringModTrackRatio(pot1);
        else                    ringmod.carrierHz = ringModCarrierHz(pot1);
        ringmod.shape = pot2;         // character = sine->tri->square
        ringmod.amBlend = 0.2f;
        ringmod.mode = m;
        ringmod.wet = wet;
        ringmod.octaveDrop = gateHi;  // IN2 = octave-down "broken speaker"
        break;

      case FX_WAVEFOLDER:
        wavefolder.foldGain = wavefolderFoldGain(pot1);
        wavefolder.offset = wavefolderOffset(pot2);  // character = symmetry
        wavefolder.toneHz = 18000.0f;
        wavefolder.wet = wet;
        // .mode is driven via setMode() (crossfade); modeOf kept in sync.
        break;

      case FX_REVERB:
        reverb.sizeScale = reverbSizeScale(pot1);
        reverb.decayGain = reverbDecayGain(pot2);  // character = decay
        reverb.damping = 0.5f;
        reverb.mode = m;
        reverb.wet = wet;
        reverb.freeze = gateHi;  // IN2 = freeze the tail
        break;

      case FX_FREEZE:
        freezeC.loopLen = freezeLoopLenSec(pot1, maxSec);
        freezeC.position = pot2;   // character = scan position
        freezeC.xfadeLen = 0.030f;
        freezeC.mode = m;
        freezeC.wet = wet;
        freezeC.freeze = gateHi;   // IN2 = freeze gate
        break;

      case FX_GLITCH:
        glitch.timeSec = clockValid
                           ? clampf(clockPeriod * glitchDelayClockRatio(pot1), 0.010f, maxSec)
                           : glitchDelayTimeSec(pot1, maxSec);
        glitch.chaos = glitchDelayChaos(pot2);
        glitch.feedback = 0.4f;
        glitch.palette = m;
        glitch.wet = wet;
        glitch.force = gateHi;  // IN2 = force a glitch/boundary
        break;

      case FX_KARPLUS:
        karplus.pitchHz = karplusPitchHz(pot1);
        karplus.damping = pot2;    // character = brightness/decay
        karplus.colour = 0.5f;
        karplus.mode = m;
        karplus.wet = wet;
        karplus.dampGate = gateHi;  // IN2 = palm-mute
        break;

      case FX_COMB:
        comb.freqHz = combFreqHz(pot1, true);
        comb.feedback = combFeedback(pot2);  // character = bipolar resonance
        comb.damping = 0.3f;
        comb.mode = m;
        comb.wet = wet;
        comb.fbKill = gateHi;  // IN2 = mute the feedback (choke)
        break;

      case FX_PITCH:
        pitch.mode = m;
        pitch.semitones = pitchShifterSemitones(pot1, true);
        pitch.detuneCents = mapClampf(pot1, 0.0f, 1.0f, 2.0f, 40.0f);
        pitch.grainSec = pitchShifterGrainSec(pot2);  // character = grain
        pitch.feedback = 0.0f;
        pitch.wet = wet;
        break;

      default: break;
    }
  }

  // Run one sample of the active core (no fade / no bypass applied here).
  float processActive(float in, float dt) {
    switch (algo) {
      case FX_DELAY:       return delay.process(in, dt);
      case FX_DISTORTION:  return distortion.process(in, dt);
      case FX_BITCRUSHER:  return bitcrusher.process(in, dt);
      case FX_CHORUS:      return chorus.process(in, dt);
      case FX_RESONATOR:   return resonator.process(in, dt);
      case FX_TAPE:        return tape.process(in, dt);
      case FX_FLANGER:     return flanger.process(in, dt);
      case FX_PHASER:      return phaser.process(in, dt);
      case FX_RINGMOD:     return ringmod.process(in, dt);
      case FX_WAVEFOLDER:  return wavefolder.process(in, dt);
      case FX_REVERB:      return reverb.process(in, dt);
      case FX_FREEZE:      return freezeC.process(in, dt);
      case FX_GLITCH:      return glitch.process(in, dt);
      case FX_KARPLUS:     return karplus.process(in, dt);
      case FX_COMB:        return comb.process(in, dt);
      case FX_PITCH:       return pitch.process(in, dt);
      default:             return in;
    }
  }

  // -----------------------------------------------------------------------
  // Main entry — advance one sample of `dt` seconds; returns the mixed output.
  // -----------------------------------------------------------------------
  float process(float in, float dt) {
    // --- clock housekeeping (only meaningful for the time-based algos) -----
    clockTimer += dt;
    if (clockTimer > kFxArenaSec * 1.5f) clockValid = false;  // clock timed out

    // --- click-free algorithm switch state machine -------------------------
    if (swState == SW_FADE_OUT) {
      fadeGain -= dt / kFadeSec;
      if (fadeGain <= 0.0f) {
        fadeGain = 0.0f;
        // Perform the swap at silence: the arena clear inside initActive()
        // happens while muted, so the (potentially long) clear can't click.
        algo = pendingAlgo;
        initActive();
        swState = SW_FADE_IN;
      }
    } else if (swState == SW_FADE_IN) {
      fadeGain += dt / kFadeSec;
      if (fadeGain >= 1.0f) {
        fadeGain = 1.0f;
        swState = SW_NORMAL;
      }
    }

    // --- map controls + run the active algorithm ---------------------------
    applyParams(dt);
    float wetOut = processActive(in, dt);

    // IN2 as a plain bypass for algorithms with no specific gate action.
    if (gateHi && fxGateIsBypass(algo)) wetOut = in;

    if (!isFiniteF(wetOut)) wetOut = 0.0f;

    // --- mute envelope over the switch (raised-cosine so it's smooth) ------
    const float g = (swState == SW_NORMAL) ? 1.0f : raisedCosine(clampf(fadeGain, 0.0f, 1.0f));
    float out = wetOut * g;

    // --- output safety + LED activity follower -----------------------------
    out = clampf(out, -1.2f, 1.2f);
    ledEnv += (fabsf(out) - ledEnv) * onePoleCoef(6.0f, dt);
    ledEnv = (ledEnv < 1e-18f) ? 0.0f : ledEnv;
    return out;
  }
};

}  // namespace sc
