/* Div Mult

Description:
Two-track clock divider AND multiplier. Each track has its own ratio dial that
sweeps one continuous range — multiplications at the left, x1 in the middle,
divisions at the right — so a track can be either, and the two tracks are
independent. A third output repeats the clock at x1 with the same pulse width.

This is a port of TWO GRAINS firmwares at once. Original Divvy (two-track clock
divider) and Multiple (two-track clock multiplier) firmwares by Sean Luke,
ported from the AE Modular GRAINS module. Fusing them is the headline change:
they are the same instrument from opposite sides — two tracks, one shared
pulsewidth pot, one clock in, one reset — and concatenating their option tables
loses nothing while reaching combinations neither could (a divided and a
multiplied track off one clock).

The two halves keep their own machinery because they really do differ.
Division is counted: a down-counter of clock edges, no notion of tempo, exact
for as long as the clock runs. Multiplication is predicted: the clock period is
measured, then subdivided, so it needs one beat to lock and it chases you for a
beat after a tempo change. Both behaviours are upstream's and both are kept.

Key Variables:
  A0 -> Track A ratio (x16 … x1 … /96, 21 positions)
  A1 -> Track B ratio (same range, independent)
  A2 -> Pulse width, shared by all three outputs

      ╔═══════════╗
      ║  DIV MULT ║
      ║   clock   ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   RATIO A — track A: x16 … x1 … /96
      ║  RATIO A  ║
      ║           ║
      ║   (A1)    ║   RATIO B — track B: x16 … x1 … /96
      ║  RATIO B  ║
      ║           ║
      ║   (A2)    ║   WIDTH   — pulse width, all outputs
      ║   WIDTH   ║
      ║           ║
      ║    [·]    ║   LED (D3) — output activity
      ║   (BTN)   ║   BTN (D4) — reset both tracks
      ║           ║
      ╠═══════════╣
      ║ F1     F2 ║   F1 (D17) IN  — Clock
      ║ (o)   (o) ║   F2 (D9)  OUT — Track A
      ║           ║
      ║ F3     F4 ║   F3 (D10) OUT — Track B
      ║ (o)   (o) ║   F4 (D11) OUT — Clock thru (x1)
      ║           ║
      ╚═══════════╝

Ratio dial, left to right (both tables verbatim from upstream, in upstream's
own order — Multiple's runs from the left edge to the middle, Divvy's from the
middle to the right edge, and they meet at the x1 this port adds):

  x16  x7  x6  x5  x4  x3  swing2  x2 | x1 | /2  /2·1  /3  /4  /4·2  /6  /8
  /16  /24  /32  /64  /96

  swing2 is Multiple's SWING_2: x3 with the middle pulse dropped, so pulses
  land on the beat and at 2/3 of it. /2·1 and /4·2 are Divvy's offset options —
  the same division started one or two clocks late, which makes them the binary
  counter digits their table was built around. /24 and /96 are MIDI clock
  quarter notes and bars; /6 is 16th notes.

Pulse width behaves as it does upstream, which is not the same thing on both
sides of the dial. A divided output rounds the pot down to whole clocks and can
never reach the full division, so there is always a falling edge before the
next pulse; with the pot at zero it sends a trigger instead. A multiplied
output takes the pot as a fraction of its own sub-beat.

Deliberate changes from the original:
  - Divvy and Multiple are one module. Their option tables are concatenated
    into a 21-position dial with a new x1 in the join, and each track chooses
    from the whole range instead of one table.
  - A third output (F4) repeats the clock at x1 with the shared pulse width.
    Neither upstream had a spare jack for it; MOD1 does.
  - Reset is the panel button, not a CV input. GRAINS reset on its AUDIO IN;
    MOD1 has one input jack and the clock needs it, so the button covers what
    the reset jack did. Reset restarts all three tracks together, exactly as
    upstream's reset() did.
  - The ratio selector has hysteresis: the pot must move a fifth of a slot into
    its neighbour before the ratio changes. Twenty-one options share one pot,
    so a pot parked on a boundary dithers on ADC noise alone, and both
    upstreams handle that badly in opposite directions — Multiple re-armed the
    track on every flip and stopped emitting sub-pulses (the "pot between two
    options may not pulse at all" weakness in its own header), while Divvy's
    OPTION_WAIT counter restarts on every flip and so never finishes counting,
    locking the pot out instead. Off a boundary the thresholds are unchanged.
  - Moving a ratio pot restarts only that track. Upstream reset both tracks
    whenever either pot moved, which here would let one pot interrupt the
    other track's phrase now that the two can be running different ratios.
    The debounce before a new ratio is accepted is upstream's OPTION_WAIT.
  - Multiplication is timed in seconds, not in loop iterations. Upstream
    counted its own main loop to estimate the beat, which ties the result to
    how long an iteration happens to take; the shared core measures the period
    and subdivides it, so the same code gives the same timing in the firmware
    and in VCV Rack.
  - A multiplied track emits one plain trigger on its first clock and starts
    subdividing on the second, once there is a period to divide. Upstream
    subdivided whatever its counter held at that moment, which is the "won't be
    right until two beats after a reset" weakness its own header calls out.
  - Zero-width pulses are 10 ms triggers. Upstream counted 100 main-loop
    iterations, which is loop-rate dependent; 10 ms is this repo's trigger
    length and reads the same on every target.
  - A full-width multiplied gate is capped just under its sub-beat so
    consecutive pulses always have an edge between them; upstream let them
    merge into a continuous high.
  - The clock input is read as a digital pin (MOD1 wiring) rather than
    threshold-compared from an analog read at 800/1023 (GRAINS wiring).

Version History:
  - 1.0 Initial release. Combined port of Sean Luke's GRAINS Divvy and
        Multiple; the engine lives in DivMultCore.h, shared with the VCV Rack
        port.

License:
Apache License 2.0. This is a port of third-party code: the original Divvy and
Multiple firmwares are Copyright 2023 Sean Luke (sean@cs.gmu.edu), from the
GRAINS project (github.com/eclab/grains), Apache 2.0. Apache requires the
license notice to travel with the code and modified files to carry prominent
notice of changes — the notice lives beside this sketch as LICENSE.md, and the
"Deliberate changes from the original" list above is the notice of changes.
Keep both.

Hardware:
HAGIWO MOD1
*/
#include <Arduino.h>
#include <Mod1Common.h>
#include <DivMultCore.h>  // shared divider/multiplier engine (also used by the VCV Rack port)

// Panel wiring
const int pinClockIn  = mod1::PIN_F1;    // external clock
const int pinOutA     = mod1::PIN_F2;    // track A
const int pinOutB     = mod1::PIN_F3;    // track B
const int pinOutThru  = mod1::PIN_F4;    // clock thru (x1)
const int pinRatioA   = mod1::PIN_POT1;
const int pinRatioB   = mod1::PIN_POT2;
const int pinWidth    = mod1::PIN_POT3;
const int pinButton   = mod1::PIN_BUTTON;
const int pinLed      = mod1::PIN_LED;

// LED brightness per track, so a glance says which output just fired. Track A
// is full, track B is dim, and the x1 thru is dimmer still — the thru fires on
// every clock and would otherwise wash the other two out.
const uint8_t kLedA    = 255;
const uint8_t kLedB    = 96;
const uint8_t kLedThru = 20;

sc::DivMultEngine engine;
mod1::EdgeInput clockEdge(LOW);
mod1::DebouncedInput resetButton(20, HIGH);

unsigned long lastMicros = 0;

void setup() {
  pinMode(pinClockIn, INPUT);
  pinMode(pinButton,  INPUT_PULLUP);
  pinMode(pinOutA,    OUTPUT);
  pinMode(pinOutB,    OUTPUT);
  pinMode(pinOutThru, OUTPUT);
  pinMode(pinLed,     OUTPUT);

  digitalWrite(pinOutA,    LOW);
  digitalWrite(pinOutB,    LOW);
  digitalWrite(pinOutThru, LOW);

  engine.reset();
  lastMicros = micros();
}

void loop() {
  const unsigned long now = micros();
  // micros() wraps every ~70 minutes; unsigned subtraction wraps with it, so
  // the elapsed time stays correct across the rollover.
  const float dt = (float)(now - lastMicros) * 1.0e-6f;
  lastMicros = now;

  // Button resets every track, which is what upstream's reset input did.
  resetButton.update((uint8_t)digitalRead(pinButton), millis());
  if (resetButton.fell()) engine.reset();

  // Ratios and pulse width. The core applies the selector hysteresis and
  // debounces a ratio change itself.
  engine.setRatioPot(0, analogRead(pinRatioA) / 1023.0f);
  engine.setRatioPot(1, analogRead(pinRatioB) / 1023.0f);
  engine.setOption(2, sc::kDivMultUnity);  // F4 is always the x1 thru
  engine.setPulseWidth(analogRead(pinWidth) / 1023.0f);

  clockEdge.update((uint8_t)digitalRead(pinClockIn));
  engine.step(dt, clockEdge.rose());

  const bool a    = engine.out(0);
  const bool b    = engine.out(1);
  const bool thru = engine.out(2);

  digitalWrite(pinOutA,    a    ? HIGH : LOW);
  digitalWrite(pinOutB,    b    ? HIGH : LOW);
  digitalWrite(pinOutThru, thru ? HIGH : LOW);

  analogWrite(pinLed, a ? kLedA : (b ? kLedB : (thru ? kLedThru : 0)));
}
