/* Tardy

Description:
Two-channel delay for triggers and gates. Whatever arrives at F1 and F2 is
re-echoed at F3 and F4 a settable moment later (up to ~853 ms), which is how
you drag a drum voice — or anything else fast — back into line with a device
that has latency. Each channel has its own delay knob; RANGE picks how far the
knobs reach, and the button links channel B to channel A's knob so one control
moves both (upstream's single shared delay). Original Tardy firmware by Sean
Luke, ported from the AE Modular GRAINS module.

Deliberate changes from the original:
  - Two channels instead of three. GRAINS delays IN1/IN2/IN3 to Audio Out /
    Digital Out / Audio In; MOD1 has four jacks, so the port is 2 in / 2 out.
    This is the headline divergence — the third channel has nowhere to go.
  - Per-channel delay knobs (POT1, POT2) where upstream had one shared delay
    pot. The button restores the shared behaviour: LINK makes channel B follow
    channel A's knob. LINK is saved to EEPROM.
  - RANGE (POT3, 3 positions) scales the delay knobs to ~107 ms / ~427 ms /
    ~853 ms full scale. Upstream is fixed at the ~853 ms setting; the short
    ranges exist because latency compensation is usually a few milliseconds and
    upstream's knob spends its whole travel getting there.
  - The tick grid is driven by elapsed time, not by how long loop() takes.
    Upstream used its own loop rate (~2404 Hz, four analogReads) as the clock,
    so the delay it produced moved with the code. The shared core advances on a
    caller-supplied dt at a fixed 2400 Hz, matching upstream's nominal rate.
  - Upstream's bittest() has an operator-precedence bug — `buffer[pos / 8] &
    (1 << (pos % 8)) != 0` is `buffer[pos / 8] & 1` — so it always read bit 0
    of the byte and seven of every eight scheduled gates were dropped. Fixed;
    the delay now resolves to a single tick (~0.42 ms) rather than 8.
  - Knob smoothing is the same 1/16 one-pole in float rather than integer, so
    the delay actually converges instead of stalling short of the setting.
  - The LED is a MOD1 addition (GRAINS has none): bright while either output is
    high, dimly lit at idle when LINK is on.

Key Variables:
  A0 -> Delay A (channel 1 delay amount)
  A1 -> Delay B (channel 2 delay amount)
  A2 -> Range (short / medium / long)

      ╔═══════════╗
      ║   TARDY   ║
      ║   delay   ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   DLY A   — channel A delay
      ║   DLY A   ║
      ║           ║
      ║   (A1)    ║   DLY B   — channel B delay (follows A when linked)
      ║   DLY B   ║
      ║           ║
      ║   (A2)    ║   RANGE   — 107 ms / 427 ms / 853 ms full scale
      ║   RANGE   ║
      ║           ║
      ║    [·]    ║   LED (D3) — output activity (dim = linked)
      ║   (BTN)   ║   BTN (D4) — link B to A's knob (saved to EEPROM)
      ║           ║
      ╠═══════════╣
      ║ F1     F2 ║   F1 (D17) IN  — trigger/gate A
      ║ (o)   (o) ║   F2 (D9)  IN  — trigger/gate B
      ║           ║
      ║ F3     F4 ║   F3 (D10) OUT — delayed A
      ║ (o)   (o) ║   F4 (D11) OUT — delayed B
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 Tardy firmware by Sean Luke for AE Modular GRAINS
  - 1.1 Ported to HAGIWO MOD1: 2 in / 2 out, per-channel delay knobs, RANGE
        selector, LINK button, activity LED
  - 1.2 Delay engine extracted to TardyCore.h (shared with the VCV Rack port);
        dt-driven tick grid and the bittest() precedence fix live there

License:
Apache License 2.0. This is a port of third-party code: the original Tardy
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
#include <EEPROM.h>
#include <Mod1Common.h>
#include <TardyCore.h>  // Shared delay engine (also used by the VCV Rack port)

#define EEPROM_ADDR_LINK 0  // Address to store the LINK toggle

const uint8_t IN_A_PIN  = mod1::PIN_F1;  // trigger/gate in A
const uint8_t IN_B_PIN  = mod1::PIN_F2;  // trigger/gate in B
const uint8_t OUT_A_PIN = mod1::PIN_F3;  // delayed A
const uint8_t OUT_B_PIN = mod1::PIN_F4;  // delayed B

const uint8_t LED_ON_LEVEL   = 255;  // LED while an output is high
const uint8_t LED_LINK_LEVEL = 20;   // LED at idle when LINK is on

sc::TardyEngine tardy;
mod1::DebouncedInput buttonDebounce(50, HIGH);

bool linkDelays = false;
uint8_t potPhase = 0;            // which pot this pass reads (round-robin)
unsigned long lastMicros = 0;

void setup() {
  // Gate inputs are plain digital reads: the delay lines only ever store a
  // level, and analogRead costs ~104 us — a third of the tick budget.
  pinMode(IN_A_PIN, INPUT);
  pinMode(IN_B_PIN, INPUT);
  pinMode(OUT_A_PIN, OUTPUT);
  pinMode(OUT_B_PIN, OUTPUT);
  pinMode(mod1::PIN_LED, OUTPUT);
  pinMode(mod1::PIN_BUTTON, INPUT_PULLUP);

  digitalWrite(OUT_A_PIN, LOW);
  digitalWrite(OUT_B_PIN, LOW);

  linkDelays = (EEPROM.read(EEPROM_ADDR_LINK) == 1);  // 0xFF (blank) = unlinked
  tardy.setLink(linkDelays);

  // Prime the smoothers from the pots so the first echo already lands at the
  // knob setting instead of sliding out from zero over the first second.
  tardy.primeDelay(0, analogRead(mod1::PIN_POT1) / 1023.0f);
  tardy.primeDelay(1, analogRead(mod1::PIN_POT2) / 1023.0f);
  tardy.setRange(mod1::select3FromAdc(analogRead(mod1::PIN_POT3)));

  lastMicros = micros();
}

void loop() {
  const unsigned long now = micros();
  const float dt = (float)(now - lastMicros) * 1e-6f;  // unsigned wrap is fine
  lastMicros = now;

  // One pot per pass. The engine wants to be called well above its 2400 Hz
  // tick rate and each analogRead is ~104 us, so scanning all three every pass
  // would eat the budget; the pots do not need 2.4 kHz.
  switch (potPhase) {
    case 0:
      tardy.setDelay01(0, analogRead(mod1::PIN_POT1) / 1023.0f);
      break;
    case 1:
      tardy.setDelay01(1, analogRead(mod1::PIN_POT2) / 1023.0f);
      break;
    default:
      tardy.setRange(mod1::select3FromAdc(analogRead(mod1::PIN_POT3)));
      break;
  }
  if (++potPhase > 2) potPhase = 0;

  // Button toggles LINK (channel B follows channel A's knob), persisted.
  buttonDebounce.update((uint8_t)digitalRead(mod1::PIN_BUTTON), millis());
  if (buttonDebounce.fell()) {
    linkDelays = !linkDelays;
    tardy.setLink(linkDelays);
    EEPROM.write(EEPROM_ADDR_LINK, linkDelays ? 1 : 0);
  }

  const bool inA = digitalRead(IN_A_PIN) == HIGH;
  const bool inB = digitalRead(IN_B_PIN) == HIGH;

  // The core banks dt and fires whole ticks; outputs only move on a tick.
  if (tardy.process(dt, inA, inB)) {
    digitalWrite(OUT_A_PIN, tardy.out[0] ? HIGH : LOW);
    digitalWrite(OUT_B_PIN, tardy.out[1] ? HIGH : LOW);

    const bool active = tardy.out[0] || tardy.out[1];
    analogWrite(mod1::PIN_LED,
                active ? LED_ON_LEVEL : (linkDelays ? LED_LINK_LEVEL : 0));
  }
}
