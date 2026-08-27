/* Geiger — three-track random trigger generator and Bernoulli gate

Description:
Every clock edge on F1, Geiger tosses a coin for each of its three trigger
outputs: POT1 sets how often F2 fires, POT2 how often F3 fires, POT3 how often
F4 fires. Fully left is silence, fully right is every clock, and in between you
get a sparse, self-similar rain of triggers — three drum tracks whose density
you dial rather than sequence. Turn POT3 to the far right and the module flips
into BERNOULLI mode: now exactly ONE output fires per clock, POT1 sets the
chance it is F2, POT2 splits what remains between F3 and F4. (POT1 at 1/3 and
POT2 at 1/2 gives an even three-way split.) Original Geiger firmware by Sean
Luke, ported from the AE Modular GRAINS module.

Key Variables:
  A0  -> Probability that output 1 (F2) fires
  A1  -> Probability that output 2 (F3) fires
  A2  -> Probability that output 3 (F4) fires; far right = BERNOULLI mode
  D17 -> Clock / trigger input (F1)
  D9  -> Trigger output 1 (F2)
  D10 -> Trigger output 2 (F3)
  D11 -> Trigger output 3 (F4)
  D4  -> Button: manual clock
  D3  -> LED: lit while any output is high

      ╔═══════════╗
      ║  GEIGER   ║
      ║  random   ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   PROB1   — probability for output 1
      ║   PROB1   ║
      ║           ║
      ║   (A1)    ║   PROB2   — probability for output 2
      ║   PROB2   ║
      ║           ║
      ║   (A2)    ║   PROB3   — probability for output 3
      ║   PROB3   ║             (far right = BERNOULLI mode)
      ║           ║
      ║    [·]    ║   LED (D3) — any trigger output
      ║   (BTN)   ║   BTN (D4) — manual clock
      ║           ║
      ╠═══════════╣
      ║ F1     F2 ║   F1 (D17) IN  — Clock / trigger
      ║ (o)   (o) ║   F2 (D9)  OUT — Trigger 1
      ║           ║
      ║ F3     F4 ║   F3 (D10) OUT — Trigger 2
      ║ (o)   (o) ║   F4 (D11) OUT — Trigger 3
      ║           ║
      ╚═══════════╝

Deliberate changes from the original:
  - the probability CV inputs are gone. GRAINS feeds IN1/IN2 into POT1/POT2 and
    still has a jack left over for the clock, because two of its three outputs
    live on pins that are not jacks (the digital-out header and IN3). MOD1 has
    exactly four jacks, and three trigger outputs plus a clock input uses all
    four: F1 doubles as CV1, and F2/F3 are outputs so CV2/CV3 read the module's
    own output. Three outputs is the module, so the CV goes. The pots keep their
    full range in exchange — upstream warns that switching a GRAINS pot to "In"
    makes it hit maximum probability at about 2 o'clock, a GRAINS wiring bug that
    simply cannot occur here.
  - every clock produces a draw. The original toggles: one clock latches the
    outputs high, the next clears them and only the clock after that draws again,
    so it answers every second clock and its gates are a whole clock period wide.
    That is a consequence of having nowhere to hang a pulse timer, not a musical
    choice — both of Sean's own descriptions ("a random trigger generator for
    three tracks", "when a trigger comes in, only one of the outputs will be
    triggered") describe per-clock behaviour. This port draws on every edge and
    emits a fixed 10 ms trigger (sc::kGeigerTriggerSec). Expect twice the event
    density of a GRAINS Geiger fed the same clock.
  - the clock input is read digitally. GRAINS clocks off AUDIO IN, an analog pin,
    so upstream thresholds at 800/1023 and requires 16 consecutive high readings
    to debounce it. F1 is a real digital input here, so a plain edge detector
    does the job with no latency floor.
  - all three outputs fire together. Upstream's output 3 sits behind the GRAINS
    audio-out low-pass filter and lags the other two slightly (its header warns
    about this); F2/F3/F4 are direct digital lines, so there is no skew.
  - the PRNG is sc::xorshift32 rather than Arduino random(), so this sketch and
    the VCV Rack port generate identical patterns from identical seeds — the
    repo-wide convention, see rack-plugins/PORTING.md.
  - the seed is mixed from 16 ADC reads across both spare CV pins instead of one
    read of A5. Upstream seeds from a single 10-bit read of a floating pin; with
    nothing patched, that is often the same value every power-up and the "random"
    pattern repeats. Sampling repeatedly and folding the readings together keeps
    the same trick but actually varies.
  - the button is new. GRAINS has no button; MOD1 does, and a manual clock is the
    obvious use for it on a module that otherwise only reacts to an external one.

Version History:
  - 1.0 Geiger firmware by Sean Luke (GRAINS, 2023)
  - 1.1 Ported to HAGIWO MOD1; engine moved to the shared GeigerCore.h

License:
Apache License 2.0. This is a port of third-party code: the original Geiger
firmware is Copyright 2023 Sean Luke (sean@cs.gmu.edu), from the GRAINS
project (github.com/eclab/grains), Apache 2.0. Apache requires the license
notice to travel with the code and modified files to carry prominent notice
of changes — the notice lives beside this sketch as LICENSE.md, and the
"Deliberate changes from the original" list above is the notice of changes.
Keep both.

Hardware:
HAGIWO MOD1
*/
#include <Arduino.h>
#include <Mod1Common.h>
#include <GeigerCore.h>  // shared random-trigger engine (also used by the VCV Rack port)

// Pin definitions
const int clockInputPin = mod1::PIN_F1;      // Clock / trigger input
const int out1Pin       = mod1::PIN_F2;      // Trigger output 1
const int out2Pin       = mod1::PIN_F3;      // Trigger output 2
const int out3Pin       = mod1::PIN_F4;      // Trigger output 3
const int prob1Pin      = mod1::PIN_POT1;    // Probability for output 1
const int prob2Pin      = mod1::PIN_POT2;    // Probability for output 2
const int prob3Pin      = mod1::PIN_POT3;    // Probability for output 3 / mode
const int buttonPin     = mod1::PIN_BUTTON;  // Manual clock
const int ledPin        = mod1::PIN_LED;     // Activity LED

// Shared engine (probability model + pulse timer)
sc::GeigerEngine geiger;

// Clock edge detection on F1
mod1::EdgeInput clockEdge(LOW);

// Button debounce; the button is wired to ground, so a press is a falling edge.
mod1::DebouncedInput buttonDebounce(20, HIGH);

// Loop timing for dt. micros(), not millis(): the loop runs in ~0.5 ms, so a
// millisecond clock floored to 1 ms would run the trigger timer 2x fast and
// shrink the 10 ms triggers below what slow downstream inputs can catch.
unsigned long lastLoopMicros = 0;

//----------------------------------------------------------------------------------
// collectSeed
// Mix ADC noise from the two CV pins into a 32-bit seed. Must run BEFORE F2/F3
// are switched to outputs: on MOD1 those jacks feed CV2/CV3 as well, so once we
// drive them the "noise" is just our own output level.
uint32_t collectSeed() {
	uint32_t seed = 0;
	for (uint8_t i = 0; i < 16; i++) {
		seed = (seed << 3) ^ (uint32_t)analogRead(mod1::PIN_CV2)
		                   ^ ((uint32_t)analogRead(mod1::PIN_CV3) << 5);
	}
	return seed ^ micros();
}

//----------------------------------------------------------------------------------
// setup
void setup() {
	pinMode(clockInputPin, INPUT);
	pinMode(buttonPin,     INPUT_PULLUP);

	geiger.seed(collectSeed());

	pinMode(out1Pin, OUTPUT);
	pinMode(out2Pin, OUTPUT);
	pinMode(out3Pin, OUTPUT);
	pinMode(ledPin,  OUTPUT);

	digitalWrite(out1Pin, LOW);
	digitalWrite(out2Pin, LOW);
	digitalWrite(out3Pin, LOW);
	digitalWrite(ledPin,  LOW);

	lastLoopMicros = micros();
}

//----------------------------------------------------------------------------------
// loop
void loop() {
	unsigned long currentMicros = micros();
	float dt = (float)(currentMicros - lastLoopMicros) / 1000000.0f;
	lastLoopMicros = currentMicros;

	// 1) Read the three probability pots (no CV — see the header's change list)
	sc::GeigerParams p = sc::geigerMapParams(
		analogRead(prob1Pin) / 1023.0f,
		analogRead(prob2Pin) / 1023.0f,
		analogRead(prob3Pin) / 1023.0f
	);

	// 2) Clock edge on F1, or a button press as a manual clock
	clockEdge.update((uint8_t)digitalRead(clockInputPin));
	buttonDebounce.update((uint8_t)digitalRead(buttonPin), millis());
	bool clockRose = clockEdge.rose() || buttonDebounce.fell();

	// 3) Draw / run the pulse timer down
	sc::GeigerResult r = geiger.process(dt, clockRose, p);

	// 4) Drive outputs
	digitalWrite(out1Pin, r.out1 ? HIGH : LOW);
	digitalWrite(out2Pin, r.out2 ? HIGH : LOW);
	digitalWrite(out3Pin, r.out3 ? HIGH : LOW);
	digitalWrite(ledPin,  r.any  ? HIGH : LOW);
}
