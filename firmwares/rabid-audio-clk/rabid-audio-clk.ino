/* The Count — rabid.audio CLK

Description:
3 HP master clock with a 3-digit 7-segment display: 15–400 BPM, two phase-locked
outputs at a settable ratio, swing, tap tempo, CV tempo modulation, pause /
single-step, and save / load / reset of the current settings to EEPROM.

Original firmware by Julian Knight

Key Variables:
  A0/A2 -> rotary encoder (quadrature, pin-change interrupt on PORTC)
  A1    -> CV in, tempo offset (0..1023 ADC -> 0..255 BPM)
  A3-A5 -> 7-segment common-cathode drives (one digit lit at a time)
  D0-D6 -> 7-segment segment lines (PORTD, addressed as a whole port)

  Their panel is jacks-at-the-top (their panel/README.md states it as house
  style) with the three display digits STACKED, so the tempo reads down the face
  rather than across it -- what a 3-digit driver costs on a 3 HP plate:

      ╔═══════════╗
      ║ the count ║
      ╠═══════════╣
      ║   (o) beat║   OUT (D10) — beat
      ║           ║
      ║   (o)  div║   OUT (D11) — subdivision / superdivision
      ║           ║
      ║ cv(o)  ↵  ║   CV  (A1)  IN  — tempo offset
      ╠═══════════╣
      ║ (·)   (·) ║   LED A (D12) beat · LED B (D13) div
      ║   ┌───┐   ║
      ║   │ 1 │   ║
      ║   │ 2 │   ║   3 x SM460281N stacked on a 10.795mm pitch,
      ║   │ 0 │   ║   multiplexed one digit at a time
      ║   └───┘   ║
      ║    bpm    ║
      ║   (O) tap ║   encoder (A0/A2) — BPM, or the held button's parameter
      ║           ║   its push-switch (D7) — tap tempo
      ║ (o) div   ║   A (D8)  — hold: encoder sets the BEAT:DIV ratio
      ║ (o) swing ║   B (D9)  — hold: encoder sets swing
      ║ div+swing ║   A+B     — toggle pause
      ╚═══════════╝

  While paused, the display shows "=" and:
    C tap    — advance one step (restart a sequencer on its downbeat)
    C hold   — 'F' full reset: restore factory settings
    A hold   — 'S' save the current settings to EEPROM
    B hold   — 'L' load the stored settings back

Version History:
  - 1.0 "The Count" clock firmware by rabid.audio
  - 1.1 Forked from https://github.com/rabidaudio/synthesizer (clock/src)
  - 1.2 Tempo/subdivision/swing engine extracted to the shared SynthCore
        ClkCore (seconds, platform-neutral); this sketch keeps only hardware I/O

Deliberate changes from the original, all documented in rack-plugins/PORTING.md:
  - the 1586-entry PROGMEM OCR1A lookup table is gone. Every entry equalled
    trunc(937500 / clock), so the engine works in seconds and this sketch scales
    by kClkTimerHz. Bit-identical compare values, ~3.2 KB of flash returned.
  - OCR1A is clamped rather than allowed to wrap. At 15 BPM with deep negative
    swing the interval reaches 5.52 s, past what a 16-bit compare can hold; the
    original wrote it anyway and the register wrapped it to 1.32 s.
  - the EEPROM sanity check accepted 15..300 BPM although the engine's ceiling is
    400, so a saved fast tempo was silently discarded. It now validates against
    the engine's real limits, and checks subdivision and swing too.
  - the subdivision range is unified at -8..4. The original's setSubdivisions
    clamped to [-4, 8] while incrementSubdivisions clamped to [-8, 4]; the engine
    keeps the range the encoder can actually reach.

License:
MIT License. Unlike the CC0 HAGIWO modules in this repo, this one is a port of
third-party code, and MIT requires the copyright and permission notice to travel
with it:

  Copyright 2015-2020 Julian Knight

The full notice is kept verbatim beside this sketch as SOFTWARE_LICENSE, copied
from the upstream repository. Keep it there — MIT permits modification and
redistribution (including commercially) only so long as that notice ships with
every copy or substantial portion of the software.

Hardware:
rabid.audio CLK ("The Count"), bare ATmega328P @ 16 MHz.
Built here for `arduino:avr:nano` — the same MCU, so the pin numbering below is
unchanged. NOTE: the segment lines occupy the whole of PORTD, which includes
D0/D1 (RX/TX). On a real Nano board those are wired to the USB-serial bridge, so
this firmware is meant for the bare-chip CLK PCB; a Nano can compile and flash it
but the two lowest segments will fight the USB bridge.

That same PORTD conflict rules out serial uploads on the real board, so the CLK
is flashed in-circuit through its ICSP header:

  make fuses-rabid-audio-clk   # ONCE per chip — 16 MHz crystal fuses
  make isp-rabid-audio-clk     # build + flash over ISP

The fuse step is not optional on a fresh chip. A factory ATmega328P runs its
internal 8 MHz RC divided by 8, and this firmware assumes 16 MHz throughout
(sc::kClkTimerHz is 16 MHz / 1024), so an unfused part keeps perfect time 16x
too slow. See `make isp-help` or the README.
*/
#include <Arduino.h>
#include <EEPROM.h>
#include <util/atomic.h>
#include <ClkCore.h>  // Shared clock engine (also used by rack-plugins/src/rabid-audio-clk.cpp)

// ── Pin map (bare ATmega328P on the CLK PCB) ───────────────────────────────
constexpr uint8_t PIN_BEAT = 10;      // OUT — beat
constexpr uint8_t PIN_DIV = 11;       // OUT — subdivision
constexpr uint8_t PIN_KNOB_A = A0;    // encoder quadrature A
constexpr uint8_t PIN_KNOB_B = A2;    // encoder quadrature B
constexpr uint8_t PIN_CV_IN = A1;     // CV — tempo offset
constexpr uint8_t PIN_LED_BEAT = 12;  // beat indicator
constexpr uint8_t PIN_LED_DIV = 13;   // div indicator
constexpr uint8_t PIN_BTN_A = 8;      // DIV   — panel button SW1
constexpr uint8_t PIN_BTN_B = 9;      // SWING — panel button SW2
constexpr uint8_t PIN_BTN_C = 7;      // TAP   — the encoder's own push-switch (SW3)

constexpr uint8_t DISPLAY_PORT = 4;  // PORTD — segment lines on its low 7 bits
constexpr uint8_t DISPLAY_CTRL_PINS[3] = {A3, A4, A5};
// To fit the driver inside 3 HP the centre digit's segment lines are permuted
// relative to its neighbours (see seg::BITS_ALT below).
constexpr bool DISPLAY_ALT_WIRING[3] = {false, true, false};

constexpr uint32_t BUTTON_DEBOUNCE_MS = 100;
constexpr uint32_t LONG_HOLD_MS = 2000;
constexpr float BPM_DISPLAY_HOLD_SEC = 1.5f;  // show base BPM this long after a turn
constexpr uint16_t LOOP_PERIOD_MS = 10;       // control rate; display keeps ticking

// ── 7-segment wiring ───────────────────────────────────────────────────────
//
// The glyphs themselves live in the shared core (sc::clkSegments, in standard
// a–g naming) so the Rack module draws exactly the characters the hardware
// shows. All that belongs here is the PCB's bit order: segments hang off the
// low 7 bits of one AVR port so a character is a single port write, and to fit
// the driver inside 3 HP the centre digit's traces are permuted relative to its
// neighbours. Each table lists which port bit carries a, b, c, d, e, f, g.
namespace seg {

//                          a  b  c  d  e  f  g
const uint8_t BITS[7] = {3, 4, 5, 6, 2, 1, 0};       // "_DCBAEFG"
const uint8_t BITS_ALT[7] = {6, 5, 4, 3, 1, 2, 0};   // "_ABCDFEG"

// Repack a shared-font mask into one digit's port bits.
inline uint8_t pack(uint8_t segments, bool altWiring) {
  const uint8_t* bits = altWiring ? BITS_ALT : BITS;
  uint8_t out = 0;
  for (uint8_t i = 0; i < 7; i++) {
    if (segments & (uint8_t)(1 << i)) out |= (uint8_t)(1 << bits[i]);
  }
  return out;
}

}  // namespace seg

// One common-cathode digit. The control pin is pulled LOW to light it.
class SevenSegment {
 public:
  void begin(uint8_t portNumber, uint8_t controlPin, bool altWiring) {
    _controlPin = controlPin;
    _altWiring = altWiring;
    _segmentPort = portOutputRegister(portNumber);
    pinMode(controlPin, OUTPUT);
    volatile uint8_t* modeReg = portModeRegister(portNumber);
    *modeReg = 0x7F;  // low 7 bits output; bit 7 left alone (may be another IO)
    turnOff();
  }

  void turnOff() { digitalWrite(_controlPin, HIGH); }
  void turnOn() { digitalWrite(_controlPin, LOW); }

  void display(char c) { write(seg::pack(sc::clkSegments(c), _altWiring)); }

 private:
  // Only the low 7 bits belong to us; bit 7 of the port is left untouched.
  void write(uint8_t bitmask) {
    *_segmentPort = (bitmask & 0b01111111) | (*_segmentPort & 0b10000000);
  }

  uint8_t _controlPin = 0;
  bool _altWiring = false;
  volatile uint8_t* _segmentPort = nullptr;
};

// Three digits sharing one segment port, lit round-robin. tick() must be called
// on a tight loop or the multiplex is visible as flicker.
class Display {
 public:
  void begin() {
    for (uint8_t i = 0; i < sc::kClkDigits; i++) {
      _digits[i].begin(DISPLAY_PORT, DISPLAY_CTRL_PINS[i], DISPLAY_ALT_WIRING[i]);
      _contents[i] = ' ';
    }
    _index = 0;
  }

  // Advance to the next digit. One digit is lit at any instant.
  void tick() {
    _digits[_index].turnOff();
    _index = (uint8_t)((_index + 1) % sc::kClkDigits);
    _digits[_index].display(_contents[_index]);
    _digits[_index].turnOn();
  }

  // Keep multiplexing for `ms` milliseconds. This is the sketch's idle loop.
  void tickFor(uint16_t ms) {
    for (uint16_t t = 0; t < ms; t++) {
      tick();
      delay(1);
    }
  }

  // `chars` is LSB-first, as produced by the ClkCore formatters.
  void show(const char chars[sc::kClkDigits]) {
    for (uint8_t i = 0; i < sc::kClkDigits; i++) _contents[i] = chars[i];
  }

  void setChar(uint8_t i, char c) {
    if (i < sc::kClkDigits) _contents[i] = c;
  }

  // Blink the whole display `count` times to acknowledge a save / load / reset.
  // Blocking, like the original: the gesture is over, nothing else is due.
  void blink(char c, uint8_t count = 3, uint8_t onMs = 50) {
    char filled[sc::kClkDigits];
    sc::clkFormatFill(filled, c);
    char blank[sc::kClkDigits];
    sc::clkFormatBlank(blank);
    for (uint8_t i = 0; i < count; i++) {
      show(blank);
      tickFor(onMs);
      show(filled);
      tickFor(onMs);
    }
  }

 private:
  SevenSegment _digits[sc::kClkDigits];  // LSB first
  char _contents[sc::kClkDigits] = {' ', ' ', ' '};
  uint8_t _index = 0;
};

// ── Panel input ────────────────────────────────────────────────────────────

// Debounced momentary button (active high), with a long-hold test.
class Button {
 public:
  void begin(uint8_t pin, uint32_t debounceMs) {
    _pin = pin;
    _debounceMs = debounceMs;
    pinMode(pin, INPUT);
    _pressed = false;
    _lastPressAt = millis();
  }

  bool isPressed() {
    const bool now = digitalRead(_pin);
    if (now == _pressed) return now;
    if (now) {  // press: act immediately
      _lastPressAt = millis();
      _pressed = true;
      return true;
    }
    // release: only believe it once the contact has settled
    const uint32_t t = millis();
    if (t < _lastPressAt || t - _lastPressAt > _debounceMs) {
      _pressed = false;
      return false;
    }
    return true;
  }

  bool isLongHold() { return isPressed() && (millis() - _lastPressAt) >= LONG_HOLD_MS; }

 private:
  uint8_t _pin = 0;
  bool _pressed = false;
  uint32_t _debounceMs = 0;
  uint32_t _lastPressAt = 0;
};

// Boxcar-averaged analog input — the CV jack is unbuffered and noisy enough that
// a raw analogRead would jitter the tempo by a BPM or two.
template <uint8_t BUFF_SIZE>
class BufferedInput {
 public:
  void begin(uint8_t pin) {
    _pin = pin;
    _index = 0;
    for (uint8_t i = 0; i < BUFF_SIZE; i++) {
      read();
      delay(2);
    }
  }

  uint16_t read() {
    _buffer[_index] = analogRead(_pin);
    _index = (uint8_t)((_index + 1) % BUFF_SIZE);
    uint16_t total = 0;
    for (uint8_t i = 0; i < BUFF_SIZE; i++) total = (uint16_t)(total + _buffer[i]);
    return (uint16_t)(total / BUFF_SIZE);
  }

 private:
  uint8_t _pin = 0;
  uint16_t _buffer[BUFF_SIZE] = {0};
  uint8_t _index = 0;
};

// Incremental rotary encoder read from a pin-change interrupt. Pins are active
// low; detents sit at the on-on state, so a gate suppresses the extra steps that
// occur between two detents.
class RotaryEncoder {
 public:
  void begin(uint8_t aPin, uint8_t bPin) {
    _aPin = aPin;
    _bPin = bPin;
    _state = 0b00001111;
    _changed = false;
    _counter = 0;
    pinMode(aPin, INPUT);
    pinMode(bPin, INPUT);
    noInterrupts();
    enablePinChange(aPin);
    enablePinChange(bPin);
    tick();
    interrupts();
  }

  // Based on https://github.com/PaulStoffregen/Encoder/blob/master/Encoder.h
  // State is {new B, new A, old B, old A}.
  inline void tick() {
    _state = (uint8_t)(_state >> 2);
    if (digitalRead(_aPin) == LOW) _state |= 0b0000100;
    if (digitalRead(_bPin) == LOW) _state |= 0b0001000;
    switch (_state) {
      case 0b00000001:
      case 0b00000111:
      case 0b00001000:
      case 0b00001110:
        if (!_changed) _counter++;
        break;
      case 0b00000010:
      case 0b00000100:
      case 0b00001011:
      case 0b00001101:
        if (!_changed) _counter--;
        break;
      default:
        break;
    }
    switch (_state) {
      case 0b00001100:
      case 0b00001101:
      case 0b00001110:
      case 0b00001111:
        _changed = false;
        break;
      case 0b00000011:
      case 0b00000111:
      case 0b00001011:
        _changed = true;
        break;
      default:
        break;
    }
  }

  // Detents accumulated since the last call.
  int8_t readChanges() {
    int8_t v;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
      v = _counter;
      _counter = 0;
    }
    return v;
  }

 private:
  static void enablePinChange(uint8_t pin) {
    *digitalPinToPCMSK(pin) |= bit(digitalPinToPCMSKbit(pin));
    PCIFR |= bit(digitalPinToPCICRbit(pin));  // clear any pending interrupt
    PCICR |= bit(digitalPinToPCICRbit(pin));  // enable the pin's group
  }

  uint8_t _aPin = 0;
  uint8_t _bPin = 0;
  uint8_t _state = 0;
  bool _changed = false;
  volatile int8_t _counter = 0;
};

// ── Persisted settings ─────────────────────────────────────────────────────
class SettingsStore {
 public:
  void begin(uint16_t baseAddress = 0) {
    _baseAddress = baseAddress;
    EEPROM.get(_baseAddress, _settings);
    if (!valid(_settings)) _settings = sc::kClkDefaults;
  }

  const sc::ClkSettings& settings() const { return _settings; }

  void write(const sc::ClkSettings& s) {
    _settings = s;
    EEPROM.put(_baseAddress, s);  // put() skips bytes that already match
  }

  void restoreDefaults() { write(sc::kClkDefaults); }

 private:
  // A blank EEPROM reads as 0xFF, so every field is range-checked against the
  // engine's real limits rather than the original's narrower 1..300 BPM guess.
  static bool valid(const sc::ClkSettings& s) {
    if (s.baseBPM < sc::kClkMinBPM || s.baseBPM > sc::kClkMaxBPM) return false;
    if (s.subdivisions < sc::kClkMinSubdiv || s.subdivisions > sc::kClkMaxSubdiv) return false;
    if (s.subdivisions == 0 || s.subdivisions == -1) return false;
    if (s.swing < sc::kClkMinSwing || s.swing > sc::kClkMaxSwing) return false;
    return true;
  }

  sc::ClkSettings _settings = sc::kClkDefaults;
  uint16_t _baseAddress = 0;
};

// ── Globals ────────────────────────────────────────────────────────────────
sc::ClkEngine clk;
sc::ClkTapTempo tapTempo;
sc::ClkPauseState pauseState;
sc::ClkChangeTimeout bpmChange;

Display display;
Button aButton, bButton, cButton;
BufferedInput<16> cvInput;
RotaryEncoder knob;
SettingsStore settingsStore;

uint32_t lastLoopMs = 0;

// The engine's periodSec / swingSec are 4-byte floats that TIMER1_COMPA reads.
// A torn read would schedule a nonsense interval, so every mutation runs with
// interrupts off. (The original protected the equivalent integers the same way.)
#define CLK_ATOMIC ATOMIC_BLOCK(ATOMIC_RESTORESTATE)

// Seconds -> Timer1 compare value, with the register's own limits applied.
//
// OCR1A is 16-bit, so the longest interval it can express is 65535 ticks
// (4.194 s). The slowest clock (15 BPM = 4.0 s) plus a deep negative swing asks
// for up to 5.52 s, which the original wrote straight into the register: it
// wrapped, and a 5.52 s interval came out as 1.32 s -- an audible tempo glitch
// at the extreme end of two controls. Clamp instead, so the swing merely stops
// deepening once the timer runs out of range.
//
// The floor keeps OCR1A clear of OCR1B (the pulse-end compare). If OCR1A ever
// fell below it the match would never fire and the outputs would latch high.
// The real range bottoms out around 364 ticks (1600 BPM at +75 % swing), so
// this is a guard rail rather than a working limit.
inline uint16_t compareFor(float intervalSec) {
  const float counts = intervalSec * sc::kClkTimerHz;
  const float floorCounts = sc::kClkPulseSec * sc::kClkTimerHz + 8.0f;
  if (counts >= 65535.0f) return 65535;
  if (counts <= floorCounts) return (uint16_t)floorCounts;
  return (uint16_t)counts;
}

// Which physical pin carries the fast line depends on the ratio's direction.
inline uint8_t fastPin() { return clk.superdivision() ? PIN_BEAT : PIN_DIV; }
inline uint8_t slowPin() { return clk.superdivision() ? PIN_DIV : PIN_BEAT; }

// The jacks belong to the ISRs while the timer is running: if the main loop also
// wrote them it could clobber a pulse the ISR had just raised (or hold one open),
// so it only ever mirrors the levels onto the LEDs. This split is the original's
// too -- its loop wrote LED_A / LED_B and nothing else.
inline void writeLeds() {
  digitalWrite(PIN_LED_BEAT, clk.beatOut() ? HIGH : LOW);
  digitalWrite(PIN_LED_DIV, clk.divOut() ? HIGH : LOW);
}

// Drive the jacks directly. Only safe with the timer interrupts disabled, i.e.
// while paused -- which is the one place the loop has to move them itself.
inline void writeJacks() {
  digitalWrite(PIN_BEAT, clk.beatOut() ? HIGH : LOW);
  digitalWrite(PIN_DIV, clk.divOut() ? HIGH : LOW);
  writeLeds();
}

// Restart the bar and re-arm Timer1 on the engine's first interval.
void resetTimer() {
  CLK_ATOMIC {
    clk.reset();
    OCR1A = compareFor(clk.firstIntervalSec());
    TCNT1 = 0;
  }
  writeJacks();
}

void setTimerEnabled(bool on) {
  if (on == clk.enabled) return;
  clk.setEnabled(on);
  if (on) {
    TIMSK1 |= (1 << OCIE1A) | (1 << OCIE1B);
  } else {
    TIMSK1 &= (uint8_t) ~((1 << OCIE1A) | (1 << OCIE1B));
  }
}

// COMPA: one clock tick. The engine picks the interval (swing alternates it) and
// advances the subdivision counter; this ISR only touches the pins and OCR1A.
ISR(TIMER1_COMPA_vect) {
  TCNT1 = 0;
  OCR1A = compareFor(clk.tick());
  digitalWrite(fastPin(), HIGH);
  if (clk.slowHigh) digitalWrite(slowPin(), HIGH);
}

// COMPB: end of the 16 ms pulse — both lines low.
ISR(TIMER1_COMPB_vect) {
  clk.endPulse();
  digitalWrite(PIN_BEAT, LOW);
  digitalWrite(PIN_DIV, LOW);
}

// Encoder quadrature lands on PORTC (A0 / A2).
ISR(PCINT1_vect) { knob.tick(); }

void setup() {
  display.begin();
  // Walk a row of dashes across the display while the peripherals come up, so a
  // hang during setup is visible as a frozen digit.
  display.setChar(0, '_');
  display.setChar(1, '_');
  display.setChar(2, '_');
  display.tick();

  pinMode(PIN_BEAT, OUTPUT);
  pinMode(PIN_DIV, OUTPUT);
  pinMode(PIN_LED_BEAT, OUTPUT);
  pinMode(PIN_LED_DIV, OUTPUT);
  digitalWrite(PIN_LED_BEAT, HIGH);
  digitalWrite(PIN_LED_DIV, HIGH);
  display.tick();

  settingsStore.begin();
  clk.loadSettings(settingsStore.settings());
  display.tick();

  // Timer1: phase-and-frequency-correct PWM with TOP = OCR1A, prescaler 1024, so
  // the counter ticks every 64 µs. COMPA is the beat, COMPB closes the pulse.
  noInterrupts();
  TCCR1A = 0;
  TCCR1B = 0;
  TCNT1 = 0;
  OCR1A = compareFor(clk.firstIntervalSec());
  OCR1B = (uint16_t)(sc::kClkPulseSec * sc::kClkTimerHz);  // 250 ticks = 16 ms
  TCCR1A |= (0 << WGM11) | (1 << WGM10);
  TCCR1B |= (1 << WGM13) | (0 << WGM12);
  TCCR1B |= (1 << CS12) | (0 << CS11) | (1 << CS10);  // /1024
  interrupts();
  clk.setEnabled(false);  // so setTimerEnabled() actually arms the interrupts
  setTimerEnabled(true);
  display.tick();

  bpmChange.begin(BPM_DISPLAY_HOLD_SEC);
  knob.begin(PIN_KNOB_A, PIN_KNOB_B);
  display.tick();
  aButton.begin(PIN_BTN_A, BUTTON_DEBOUNCE_MS);
  bButton.begin(PIN_BTN_B, BUTTON_DEBOUNCE_MS);
  cButton.begin(PIN_BTN_C, BUTTON_DEBOUNCE_MS);
  display.tick();
  cvInput.begin(PIN_CV_IN);
  display.tick();

  digitalWrite(PIN_LED_BEAT, LOW);
  digitalWrite(PIN_LED_DIV, LOW);
  char blank[sc::kClkDigits];
  sc::clkFormatBlank(blank);
  display.show(blank);
  lastLoopMs = millis();
}

void loop() {
  const uint32_t now = millis();
  const float dt = (float)(now - lastLoopMs) / 1000.0f;
  lastLoopMs = now;

  const bool aPressed = aButton.isPressed();
  const bool bPressed = bButton.isPressed();
  const bool cPressed = cButton.isPressed();
  const int8_t knobMotion = knob.readChanges();

  bpmChange.tick(dt);
  // Tap tempo has to see every loop, not just the ones where C is held.
  const uint16_t tapBpm = tapTempo.tick(dt, cPressed);

  // CV: 0..1023 ADC scaled to a 0..255 BPM offset, as on the original.
  const uint16_t cvOffset = (uint16_t)(cvInput.read() / 4);

  pauseState.setState(aPressed && bPressed);
  const bool paused = pauseState.isPaused();

  char digits[sc::kClkDigits];

  CLK_ATOMIC { clk.setBPMOffset(cvOffset); }
  setTimerEnabled(!paused);

  if (paused) {
    // Frozen: hold the bar at its start so the next run begins on a downbeat.
    resetTimer();
    tapTempo.cancel();
    sc::clkFormatPaused(digits);

    if (pauseState.isTap(cPressed)) {
      // Nudge a stopped sequencer forward by exactly one step.
      CLK_ATOMIC { clk.singleTick(); }
      writeJacks();
      display.tickFor((uint16_t)(sc::kClkPulseSec * 1000.0f));
      CLK_ATOMIC { clk.endPulse(); }
      writeJacks();
    } else if (cPressed) {
      sc::clkFormatFill(digits, 'F');  // full reset
      if (cButton.isLongHold()) {
        settingsStore.restoreDefaults();
        CLK_ATOMIC { clk.loadSettings(settingsStore.settings()); }
        display.show(digits);
        display.blink('F');
        resetTimer();
      }
    } else if (aPressed) {
      sc::clkFormatFill(digits, 'S');  // save
      if (aButton.isLongHold()) {
        settingsStore.write(clk.currentSettings());
        display.show(digits);
        display.blink('S');
      }
    } else if (bPressed) {
      sc::clkFormatFill(digits, 'L');  // load
      if (bButton.isLongHold()) {
        CLK_ATOMIC { clk.loadSettings(settingsStore.settings()); }
        display.show(digits);
        display.blink('L');
        resetTimer();
      }
    }
  } else if (aPressed) {
    int8_t subdiv;
    CLK_ATOMIC { subdiv = clk.incrementSubdivisions(knobMotion); }
    sc::clkFormatSubdiv(digits, subdiv);
  } else if (bPressed) {
    // Swing moves three points per detent — the full ±75 range in half a turn.
    int8_t swing;
    CLK_ATOMIC { swing = clk.incrementSwing((int16_t)(3 * knobMotion)); }
    sc::clkFormatNumber(digits, swing);
  } else if (cPressed && tapTempo.isActive()) {
    CLK_ATOMIC { clk.setBaseBPM(tapBpm); }
    bpmChange.noteChanged();
    sc::clkFormatNumber(digits, (int16_t)tapBpm);
  } else {
    uint16_t base;
    CLK_ATOMIC { base = clk.incrementBaseBPM(knobMotion); }
    if (knobMotion != 0) bpmChange.noteChanged();
    // While the encoder is moving show the tempo being dialled in; once it
    // settles, show the true rate including the CV offset.
    sc::clkFormatNumber(digits, (int16_t)(bpmChange.isChanging() ? base : clk.bpm()));
  }

  display.show(digits);
  writeLeds();
  display.tickFor(LOOP_PERIOD_MS);
}
