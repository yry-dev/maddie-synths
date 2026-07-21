/* GLITCH DELAY

Description:
A delay whose read head misbehaves on purpose — random skips, reversed chunks,
in-place stutters and tape-cut splices. Audio comes in on the CV jack (sampled
by the RP2350 ADC inside the ~36.6 kHz PWM ISR, per firmwares/mod2-fx/README.md)
and a 293 KB SRAM arena gives up to 4 s of delay. The read side is chopped into
chunks (one chunk = the POT1 delay time); at each chunk boundary a die is rolled
against POT2 "chaos" and, per the selected palette, the next chunk is rendered
as a plain repeat, a skip to a random offset, a reversed read, a stuttered
re-read, or a half/double-speed tape-cut. Two read heads with a ~3 ms
raised-cosine crossfade make every jump click-free. The per-chunk RNG reseeds
from the chunk index modulo a 16-chunk loop, so over a repeating musical loop
the same chunks glitch the same way every pass (great for live sets). A clock on
IN1 quantises chunk boundaries to musical divisions of the period ({1/4, 1/3,
1/2, 2/3, 3/4, 1, 1.5, 2}x picked by POT1). IN2 is a force-glitch gate
(guaranteed event while high). Short button presses cycle the glitch palette
(Skips / Reverse / Stutter / All); long presses (>=0.6 s) tap the tempo. Hold
BUTTON + turn POT1 for wet/dry, BUTTON + turn POT2 for feedback (both persisted
to flash with the palette). DSP lives in the shared sc::GlitchDelayCore (also
used by the VCV Rack port).

Key Variables:
  A0 -> Delay time / chunk length (20 ms - 4 s) / clock division (BTN held: wet/dry)
  A1 -> Chaos (glitch probability + intensity)         (BTN held: feedback)
  A2 -> AUDIO INPUT (POT3 is therefore unavailable)

      ╔═══════════╗
      ║  GLITCH   ║
      ║    fx     ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   POT1 (A0) - delay time / clock division (BTN held: wet/dry)
      ║   TIME    ║
      ║           ║
      ║   (A1)    ║   POT2 (A1) - chaos amount (BTN held: feedback)
      ║   CHAOS   ║
      ║           ║
      ║   (A2)    ║   POT3 (A2) - unavailable (pin shared with audio in)
      ║    ---    ║
      ║           ║
      ║    [·]    ║   LED (GPIO5) - flashes on each glitch event (blinks palette ID)
      ║   (BTN)   ║   BTN (GPIO6) - short: palette; long: tap; hold+POT1/2: wet/fb
      ║           ║
      ╠═══════════╣
      ║ I1     I2 ║   IN1 (GPIO7) - clock in (chunk boundaries lock to divisions)
      ║ (o)   (o) ║   IN2 (GPIO0) - force-glitch gate (HIGH = guaranteed event)
      ║           ║
      ║ OUT    CV ║   CV  (A2)    - AUDIO INPUT (~36.6 kHz ADC sampling)
      ║ (o)   (o) ║   OUT (GPIO1) - PWM audio (~36.6 kHz)
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 Initial glitch-delay firmware (maddie synths original, shared GlitchDelayCore)

License:
CC0 1.0 Universal (CC0 1.0) Public Domain Dedication
You can copy, modify, distribute and perform the work, even for commercial
purposes, all without asking permission.

Hardware:
HAGIWO MOD2 (Seeed Xiao RP2350)
*/

#include <Arduino.h>
#include "hardware/pwm.h"
#include "hardware/irq.h"
#include <math.h>
#include <EEPROM.h>            // persisted wet/dry + feedback + palette
#include <Mod2Common.h>        // Shared MOD2 pin map, PWM-audio setup and helpers
#include <GlitchDelayCore.h>   // Shared glitch DSP (also used by rack-plugins/src/mod2-glitch-delay.cpp)

/* ============================== constants ============================== */
constexpr float POT_MOVE_THRESHOLD = 0.04f;   // pot travel that counts as "moved"
constexpr uint32_t SHORT_PRESS_MS  = 600;     // below = palette toggle, above = tap
constexpr uint32_t SAVE_DELAY_MS   = 1500;    // settle time before a flash save
constexpr float MAX_DELAY_SEC      = 4.0f;    // arena-limited maximum time
constexpr uint32_t TAP_MAX_MS      = 4000;    // longest usable tap interval
// A clock is "present" while the last IN1 tick is under 4 periods old.
constexpr uint32_t CLOCK_MAX_PERIOD = (uint32_t)(MAX_DELAY_SEC * mod2::AUDIO_FS);

/* ------------------------- persisted settings ------------------------- */
struct Settings {
  uint32_t magic;
  float wet;
  float feedback;
  uint8_t palette;
};
constexpr uint32_t SETTINGS_MAGIC = 0x474C4431;  // "GLD1"

/* ========================== hardware globals =========================== */
uint sliceAudio;  // PWM slice for audio out
uint sliceIRQ;    // PWM slice that triggers the ISR
uint sliceLed;    // PWM slice for LED brightness

/* -------------------- shared DSP core --------------------------------- */
// 293 KB int16 arena in SRAM = 4.0 s at ~36.6 kHz (RP2350 has 520 KB).
static int16_t glitchArena[(uint32_t)(MAX_DELAY_SEC * mod2::AUDIO_FS) + 16];  // +16 matches the Rack port's arena
sc::GlitchDelayCore glitchFx;
sc::DcBlocker dcBlock;  // removes the ADC's unipolar bias from the audio in

/* Volatile shadows: loop() writes, ISR reads (same pattern as mod2-delay). */
volatile float   g_timeSec  = 0.4f;
volatile float   g_chaos    = 0.4f;
volatile float   g_feedback = 0.4f;
volatile float   g_wet      = 0.5f;
volatile uint8_t g_palette  = sc::GLITCH_ALL;
volatile int16_t g_ledForce = -1;  // >=0: loop-driven blink level, -1: event flash

/* ISR -> loop: IN1 clock measurement. */
volatile uint32_t g_clockPeriod = 0;          // samples between the last two ticks
volatile uint32_t g_samplesSinceTick = CLOCK_MAX_PERIOD;

/* ISR-only state. */
static bool isrLastIn1 = false;

/* =======================================================================
 *  PWM interrupt service routine (~36.6 kHz)
 * ==================================================================== */
void __isr onPwmWrap()
{
  /* --- audio input: sample the CV jack at the audio rate ------------- */
  const int raw = analogRead(mod2::CV_PIN);
  const float in = dcBlock.process(raw * (2.0f / 1023.0f) - 1.0f);

  /* --- IN1: clock period measurement (sample-accurate edges) --------- */
  const bool in1 = digitalRead(mod2::IN1_PIN) == HIGH;
  if (in1 && !isrLastIn1) {
    if (g_samplesSinceTick < CLOCK_MAX_PERIOD)
      g_clockPeriod = g_samplesSinceTick;
    g_samplesSinceTick = 0;
  } else if (g_samplesSinceTick < 0xFFFFFFF0u) {
    g_samplesSinceTick++;
  }
  isrLastIn1 = in1;

  /* --- glitch delay --------------------------------------------------- */
  glitchFx.timeSec  = g_timeSec;
  glitchFx.chaos    = g_chaos;
  glitchFx.feedback = sc::glitchDelayFeedback(g_feedback);  // raw pot -> 0..0.98
  glitchFx.wet      = g_wet;
  glitchFx.palette  = g_palette;
  glitchFx.force    = digitalRead(mod2::IN2_PIN) == HIGH;  // force-glitch gate
  float out = glitchFx.process(in, 1.0f / mod2::AUDIO_FS);

  out = sc::clampf(out, -1.0f, 1.0f);
  pwm_set_chan_level(sliceAudio, PWM_CHAN_B,
                     (uint16_t)((out + 1.0f) * mod2::PWM_MID + 0.5f));

  /* --- LED: flashes on each glitch event, unless loop() is blinking a
         palette ID after a mode change ------------------------------- */
  const int16_t force = g_ledForce;
  pwm_set_chan_level(sliceLed, PWM_CHAN_B,
                     force >= 0 ? (uint16_t)force
                                : (uint16_t)(glitchFx.ledLevel() * mod2::PWM_FS));

  pwm_clear_irq(sliceIRQ);
}

/* =======================================================================
 *  Guarded pot read — the ISR reads A2 through the same ADC mux (see the
 *  mod2-delay note); blocking the IRQ for the ~2 us conversion just delays
 *  one audio sample slightly.
 * ==================================================================== */
static float readPotGuarded(uint8_t pin)
{
  noInterrupts();
  const int v = analogRead(pin);
  interrupts();
  return v / 1023.0f;
}

/* =======================================================================
 *  Setup
 * ==================================================================== */
void setup()
{
  pinMode(A0, INPUT);                       // time / wet-dry (shifted)
  pinMode(A1, INPUT);                       // chaos / feedback (shifted)
  pinMode(A2, INPUT);                       // audio input
  pinMode(mod2::IN1_PIN, INPUT);            // clock in
  pinMode(mod2::IN2_PIN, INPUT);            // force-glitch gate
  pinMode(mod2::BUTTON_PIN, INPUT_PULLUP);  // palette / tap / shift button
  analogReadResolution(10);

  /* --- persisted settings -------------------------------------------- */
  EEPROM.begin(64);
  Settings s;
  EEPROM.get(0, s);
  if (s.magic == SETTINGS_MAGIC) {
    g_wet = sc::clampf(s.wet, 0.0f, 1.0f);
    g_feedback = sc::clampf(s.feedback, 0.0f, 1.0f);
    g_palette = s.palette < sc::GLITCH_PALETTE_COUNT ? s.palette : sc::GLITCH_ALL;
  }

  glitchFx.init(glitchArena, sizeof(glitchArena) / sizeof(glitchArena[0]));

  /* --- LED PWM + audio PWM + ~36.6 kHz wrap-IRQ (shared setup) ------- */
  sliceLed = mod2::initPwmOutput10bit(mod2::LED_PIN);
  mod2::initAudioPwm(sliceAudio, sliceIRQ, onPwmWrap);
}

/* =======================================================================
 *  Main loop — pots, button (palette / tap / wet+fb shift), clock sync,
 *  palette-ID LED blink, flash saves
 * ==================================================================== */
void loop()
{
  /* --- pots, one-pole smoothed ---------------------------------------- */
  static float pot1 = 0.0f, pot2 = 0.0f;
  static bool primed = false;
  const float r1 = readPotGuarded(mod2::POT1_PIN);
  const float r2 = readPotGuarded(mod2::POT2_PIN);
  if (!primed) {
    pot1 = r1;
    pot2 = r2;
    primed = true;
  }
  pot1 += (r1 - pot1) * 0.2f;
  pot2 += (r2 - pot2) * 0.2f;

  /* --- button: short = palette, long = tap, hold+POT1/2 = wet/fb ------ */
  static bool lastPressed = false;
  static bool adjustingWet = false;   // POT1 borrowed for wet/dry
  static bool adjustingFb = false;    // POT2 borrowed for feedback
  static bool tapRegistered = false;
  static uint32_t pressStartMs = 0;
  static uint32_t lastTapStartMs = 0;
  static float pot1AtPress = 0.0f;
  static float pot2AtPress = 0.0f;
  static float tapTimeSec = 0.0f;         // 0 = no tap override active
  static mod2::PickupParam timePickup;    // POT1 -> time after a wet shift
  static mod2::PickupParam chaosPickup;   // POT2 -> chaos after a feedback shift
  static bool dirty = false;
  static uint32_t lastChangeMs = 0;

  const bool pressed = digitalRead(mod2::BUTTON_PIN) == LOW;
  const uint32_t now = millis();

  /* blink state (palette-ID feedback: palette+1 flashes) */
  static uint8_t blinkPhasesLeft = 0;
  static uint32_t blinkNextMs = 0;

  if (pressed && !lastPressed) {  // press edge
    pressStartMs = now;
    pot1AtPress = pot1;
    pot2AtPress = pot2;
    adjustingWet = false;
    adjustingFb = false;
    tapRegistered = false;
  }
  /* shift layer: POT1 -> wet, POT2 -> feedback (engaged on first movement) */
  if (pressed && !adjustingWet && fabsf(pot1 - pot1AtPress) > POT_MOVE_THRESHOLD)
    adjustingWet = true;
  if (pressed && !adjustingFb && fabsf(pot2 - pot2AtPress) > POT_MOVE_THRESHOLD)
    adjustingFb = true;
  if (pressed && adjustingWet) {
    g_wet = pot1;
    dirty = true;
    lastChangeMs = now;
  }
  if (pressed && adjustingFb) {
    g_feedback = pot2;
    dirty = true;
    lastChangeMs = now;
  }
  const bool anyShift = adjustingWet || adjustingFb;

  /* Long press = tap (only when POT1 wasn't borrowed for a shift). Timestamped
     at the press EDGE so the rhythm isn't release-dependent. */
  if (pressed && !anyShift && !tapRegistered &&
      now - pressStartMs >= SHORT_PRESS_MS) {
    tapRegistered = true;
    const uint32_t interval = pressStartMs - lastTapStartMs;
    lastTapStartMs = pressStartMs;
    if (interval >= SHORT_PRESS_MS && interval <= TAP_MAX_MS) {
      tapTimeSec = interval * 0.001f;
      /* POT1 gives way to the tap until it moves past where it sits now. */
      timePickup.targetValue = pot1;
      timePickup.lastPotValue = pot1;
      timePickup.pickupActive = true;
    }
  }
  if (!pressed && lastPressed) {  // release edge
    if (adjustingWet) {
      /* POT1 was borrowed for wet/dry: freeze the time until the pot is
         turned back across where it sat before the press.               */
      timePickup.targetValue = pot1AtPress;
      timePickup.lastPotValue = pot1;
      timePickup.pickupActive = true;
    }
    if (adjustingFb) {
      /* POT2 was borrowed for feedback: freeze chaos until POT2 returns. */
      chaosPickup.targetValue = pot2AtPress;
      chaosPickup.lastPotValue = pot2;
      chaosPickup.pickupActive = true;
    }
    if (!anyShift && !tapRegistered && now - pressStartMs < SHORT_PRESS_MS) {
      g_palette = (g_palette + 1) % sc::GLITCH_PALETTE_COUNT;
      dirty = true;
      lastChangeMs = now;
      blinkPhasesLeft = (uint8_t)((g_palette + 1) * 2);  // on/off phases
      blinkNextMs = now;
    }
  }
  lastPressed = pressed;

  /* --- POT2 -> chaos (unless shifted / waiting for pickup) ------------- */
  if (!pressed) {
    if (mod2::checkPickup(chaosPickup, pot2))
      g_chaos = sc::glitchDelayChaos(pot2);
    chaosPickup.lastPotValue = pot2;
  }

  /* --- delay time / chunk length: clock > tap > POT1 ------------------ */
  noInterrupts();
  const uint32_t clockPeriod = g_clockPeriod;
  const uint32_t sinceTick = g_samplesSinceTick;
  interrupts();
  const bool clocked = clockPeriod > 0 && sinceTick < clockPeriod * 4 &&
                       sinceTick < CLOCK_MAX_PERIOD;

  if (!pressed) {
    if (mod2::checkPickup(timePickup, pot1))
      tapTimeSec = 0.0f;  // knob reclaimed: back to manual time
    timePickup.lastPotValue = pot1;
  }

  if (clocked) {
    /* POT1 picks a musical ratio of the measured period. */
    const float periodSec = clockPeriod / mod2::AUDIO_FS;
    g_timeSec = sc::clampf(periodSec * sc::glitchDelayClockRatio(pot1),
                           0.020f, MAX_DELAY_SEC);
  } else if (tapTimeSec > 0.0f) {
    g_timeSec = tapTimeSec;
  } else if (!pressed && !timePickup.pickupActive) {
    g_timeSec = sc::glitchDelayTimeSec(pot1, MAX_DELAY_SEC);
  }

  /* --- palette-ID LED blink code (event flash resumes after) ---------- */
  if (blinkPhasesLeft > 0) {
    if (now >= blinkNextMs) {
      blinkPhasesLeft--;
      g_ledForce = (blinkPhasesLeft & 1) ? (int16_t)mod2::PWM_FS : 0;
      blinkNextMs = now + 120;
      if (blinkPhasesLeft == 0)
        g_ledForce = -1;  // back to event flashes
    }
  }

  /* --- debounced flash save (commit stalls the audio ISR for a few ms,
         so only save once the panel has settled) ----------------------- */
  if (dirty && !pressed && now - lastChangeMs > SAVE_DELAY_MS) {
    Settings s = {SETTINGS_MAGIC, g_wet, g_feedback, g_palette};
    EEPROM.put(0, s);
    EEPROM.commit();
    dirty = false;
  }
}
