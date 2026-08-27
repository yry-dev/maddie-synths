/* Braids

Description:
Mutable Instruments Braids macro-oscillator port for the Seeed XIAO
RP2350 - 47 synthesis engines selectable via the button. Timbre and Morph
shape the current engine; pitch tracks V/oct (shared with CV). The trigger
input fires a note and lights the LED. 48 kHz PWM audio. GPIO23 is driven
HIGH to force the SMPS into PWM mode (reduces audio ripple).
NOTE: this port uses its own pin map (OUT=D7, TRIG=D5, BTN=D4, LED=13),
which differs from the standard MOD2 panel pins.

Key Variables:
  A0 -> Timbre (engine timbre, mapped 0-32767)
  A1 -> Morph (engine morph/colour, mapped 0-32767)
  A2 -> Pitch / V-Oct (inverted, shared with CV)

      ╔═══════════╗
      ║  BRAIDS   ║
      ║  MI port  ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   POT1 (A0) - engine timbre
      ║  TIMBRE   ║
      ║           ║
      ║   (A1)    ║   POT2 (A1) - engine morph / colour
      ║   MORPH   ║
      ║           ║
      ║   (A2)    ║   POT3 (A2) - pitch / V-Oct
      ║   PITCH   ║
      ║           ║
      ║    [·]    ║   LED (13) - follows gate / trigger
      ║   (BTN)   ║   BTN (D4) - engine select (short=next, long=prev,
      ║           ║              hold 10 s = reset to the first engine and
      ║           ║              flash the LED 5 times). The selected engine
      ║           ║              is saved to flash and restored on power-up.
      ║           ║
      ╠═══════════╣
      ║ I1     I2 ║   IN1 (D5)  - trigger / gate (fires note)
      ║ (o)   (o) ║   IN2 (SCL) - reserved / unused
      ║           ║
      ║ OUT   CV  ║   CV  (A2)  - V/oct (shared with POT3)
      ║ (o)   (o) ║   OUT (D7)  - PWM audio (48 kHz)
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 Mutable Instruments Braids port by blueprint@poetaster.de
  - 1.1 Forked and refactored for maddie synths
  - 1.2 Engine selection persisted to flash (EEPROM emulation, debounced
        commit); 10 s button hold resets to the first engine with 5 LED
        flashes. Boot default is now engine 0 (CSAW) on a fresh chip; a saved
        selection always wins.

License:
GPLv3. (c) 2025 blueprint@poetaster.de
Port of Mutable Instruments Braids; the bundled MI libraries are MIT-licensed,
as the original STM sources from Mutable Instruments were also MIT.

Hardware:
HAGIWO MOD2 (Seeed Xiao RP2350)
*/

/* toepler +
    // Toepler Plus pins
  #define OUT1 (0u)
  #define OUT2 (1u)
  #define SW1 (8u)
  #define CV1 (26u)
  #define CV2 (27u)
  #define CV3 (28u)

*/
bool debug = true;  // Enable debug output for engine information

#include <Arduino.h>
#include "stdio.h"
#include "pico/stdlib.h"
#include "hardware/sync.h"
#include "potentiometer.h"

//#include <MIDI.h>
//#include <mozzi_midi.h>

long midiTimer;

float pitch_offset = 36;
float max_voltage_of_adc = 3.3;
float voltage_division_ratio = 0.3333333333333;
float notes_per_octave = 12;
float volts_per_octave = 1;

float mapping_upper_limit = (max_voltage_of_adc / voltage_division_ratio) * notes_per_octave * volts_per_octave;
/*

  struct Serial1MIDISettings : public midi::DefaultSettings
  {
  static const long BaudRate = 31250;
  static const int8_t TxPin  = 12u;
  static const int8_t RxPin  = 13u;
  };

  MIDI_CREATE_CUSTOM_INSTANCE(HardwareSerial, Serial1, MIDI, Serial1MIDISettings);
*/

#include <hardware/pwm.h>
#include <PWMAudio.h>

#define SAMPLERATE 48000
//#define PWMOUT A0
#define PWMOUT D7
#define BUTTON_PIN D4 // D8 on the seeed board
#define TRIG_PIN D5 // D7 on mod2?
#define LED 13

#include "utility.h"
#include <STMLIB.h>
#include <BRAIDS.h>
#include "braids.h"


#include <Bounce2.h>
Bounce2::Button button = Bounce2::Button();

// ── Engine persistence (RP2040 core flash-backed EEPROM emulation) ─────────
// The chosen engine survives a power cycle. Writes are debounced: the save
// lands only after the engine has been stable for a moment, because
// EEPROM.commit() is a flash write and stalls both cores for a few ms (the
// same convention as the other MOD2 firmwares). Layout: 'B','R', version,
// engine.
#include <EEPROM.h>
#define DEFAULT_ENGINE 0          // "first engine" — CSAW; also the 10 s reset target
#define ENGINE_RESET_MS 10000     // hold this long to reset to DEFAULT_ENGINE
#define SAVE_DEBOUNCE_MS 1500     // engine must sit still this long before commit
const uint8_t kSaveMagic0 = 'B', kSaveMagic1 = 'R', kSaveVersion = 1;
bool engineDirty = false;
uint32_t engineDirtyMs = 0;
bool resetHandled = false;        // one reset per hold

PWMAudio DAC(PWMOUT);  // 16 bit PWM audio

// Braids engine names (47 total, 0-46)
const char* engineNames[47] = {
  "CSAW",      // 0 - Classic sawtooth
  "MORPH",     // 1 - Morphing saw/square
  "SAW_SQ",    // 2 - Saw and square mix
  "FOLD",      // 3 - Wavefolding
  "SQ_SUB",    // 4 - Square with sub
  "SAW_SUB",   // 5 - Sawtooth with sub
  "SQ_SYNC",   // 6 - Square sync
  "SAW_3",     // 7 - Sawtooth swarm (3x)
  "SQ_3",      // 8 - Square swarm (3x)
  "SAW_COMB",  // 9 - Sawtooth comb
  "TOY",       // 10 - Toy synthesizer
  "ZLPF",      // 11 - Low-pass filtered saw
  "ZPKF",      // 12 - Peak filtered saw
  "ZBPF",      // 13 - Band-pass filtered saw
  "ZHPF",      // 14 - High-pass filtered saw
  "VOSIM",     // 15 - VOSIM formant
  "VOWEL",     // 16 - Vowel synthesis
  "VOW_FOF",   // 17 - FOF vowel synthesis
  "HARM",      // 18 - Harmonic oscillator
  "FM",        // 19 - 2-operator FM
  "FBFM",      // 20 - Feedback FM
  "WTFM",      // 21 - Wavetable FM
  "PLUCK",     // 22 - Karplus-Strong plucked string
  "BOW",       // 23 - Bowed string
  "BLOW",      // 24 - Blown pipe
  "FLUTE",     // 25 - Flute model
  "BELL",      // 26 - Bell/metallic
  "DRUM",      // 27 - Drum model
  "KICK",      // 28 - Kick drum
  "CYMBAL",    // 29 - Cymbal
  "SNARE",     // 30 - Snare drum
  "WTBL",      // 31 - Wavetable
  "WMAP",      // 32 - Wavemap
  "WLIN",      // 33 - Wave terrain
  "WTx4",      // 34 - 4x wavetable
  "NOISE",     // 35 - Particle noise
  "TWNQ",      // 36 - Twin peaks noise
  "CLKN",      // 37 - Clocked noise
  "CLOUD",     // 38 - Granular cloud
  "PRTC",      // 39 - Particle system
  "QPSK",      // 40 - Digital modulation
  "ENG41",     // 41 - Engine 41
  "ENG42",     // 42 - Engine 42
  "ENG43",     // 43 - Engine 43
  "ENG44",     // 44 - Engine 44
  "ENG45",     // 45 - Engine 45
  "ENG46"      // 46 - Engine 46
};

int engineCount = 0;
int engineInc = 0;
bool longPressHandled = false;

// clock timer  stuff

#define TIMER_INTERRUPT_DEBUG         0
#define _TIMERINTERRUPT_LOGLEVEL_     4

// Can be included as many times as necessary, without `Multiple Definitions` Linker Error
#include "RPi_Pico_TimerInterrupt.h"

//unsigned int SWPin = CLOCKIN;

#define TIMER0_INTERVAL_MS 20.833333333333

//24.390243902439025 // 44.1
// \20.833333333333 running at 48Khz
// 10.416666666667  96kHz

#define DEBOUNCING_INTERVAL_MS   2// 80
#define LOCAL_DEBUG              0

volatile int counter = 0;

// Init RPI_PICO_Timer, can use any from 0-15 pseudo-hardware timers
RPI_PICO_Timer ITimer0(0);

bool TimerHandler0(struct repeating_timer *t) {
  (void) t;
  bool sync = true;
  if ( DAC.availableForWrite()) {
    for (size_t i = 0; i < BLOCK_SIZE; i++) {
      DAC.write( voices[0].pd.buffer[i], sync);
    }
    counter =  1;
  }

  return true;
}

void cb() {
  bool sync = true;
  if (DAC.availableForWrite() >= BLOCK_SIZE) {
    for (int i = 0; i <  BLOCK_SIZE; i++) {
      // out = ;   // left channel called .aux
      DAC.write( voices[0].pd.buffer[i]);
    }
  }
}

void HandleNoteOn(byte channel, byte note, byte velocity) {
  pitch_in = note << 7;
  trigger_in = velocity / 127.0;

  //aSin.setFreq(mtof(float(note)));
  //envelope.noteOn();
  //digitalWrite(LED, HIGH);
}
void HandleNoteOff(byte channel, byte note, byte velocity) {

  trigger_in = 0.0f;

  //aSin.setFreq(mtof(float(note)));
  //envelope.noteOn();
  //digitalWrite(LED, LOW);
}

void setup() {

  if (debug) {
    Serial.begin(57600);
    Serial.println(F("=== MOD2 BRAIDS FIRMWARE ==="));
  }

  // Engine: restore the saved selection, or DEFAULT_ENGINE on a fresh chip.
  // begin() here on core 0, before setup1() launches, so the RAM mirror is
  // ready before anything on core 1 can touch it.
  EEPROM.begin(64);
  engineCount = DEFAULT_ENGINE;
  if (EEPROM.read(0) == kSaveMagic0 && EEPROM.read(1) == kSaveMagic1 &&
      EEPROM.read(2) == kSaveVersion) {
    const uint8_t saved = EEPROM.read(3);
    if (saved <= 46)
      engineCount = saved;
  }
  engine_in = engineCount;
  if (debug) {
    Serial.print(F("Engine at boot: "));
    Serial.print(engineCount);
    Serial.print(F(" - "));
    Serial.println(engineNames[engineCount]);
  }

  analogReadResolution(12);
  // thi is to switch to PWM for power to avoid ripple noise
  pinMode(23, OUTPUT);
  digitalWrite(23, HIGH);
  
  pinMode(TRIG_PIN, INPUT_PULLDOWN);
  pinMode(AIN0, INPUT);
  pinMode(AIN1, INPUT);
  pinMode(AIN2, INPUT);
  pinMode(SCL, INPUT_PULLDOWN);

  pinMode(LED, OUTPUT);
  //MIDI.setHandleNoteOn(HandleNoteOn);  // Put only the name of the function
  //MIDI.setHandleNoteOff(HandleNoteOff);  // Put only the name of the function
  // Initiate MIDI communications, listen to all channels (not needed with Teensy usbMIDI)
  //MIDI.begin(MIDI_CHANNEL_OMNI);

  button.attach( BUTTON_PIN , INPUT_PULLUP);
  button.interval(5);
  button.setPressedState(LOW);

  // pwm timing setup, we're using a pseudo interrupt

  if (ITimer0.attachInterruptInterval(TIMER0_INTERVAL_MS, TimerHandler0)) // that's 48kHz
  {
    if (debug) Serial.print(F("Starting  ITimer0 OK, millis() = ")); Serial.println(millis());
  }  else {
    if (debug) Serial.println(F("Can't set ITimer0. Select another freq. or timer"));
  }


  // set up Pico PWM audio output
  DAC.setBuffers(4, 32); // plaits::kBlockSize); // DMA buffers
  //DAC.onTransmit(cb);
  DAC.setFrequency(SAMPLERATE);
  DAC.begin();


  // init the braids voices
  initVoices();

  // Force oscillator to use our default engine (initVoices sets VOWEL_FOF)
  voices[0].pd.osc->set_shape(static_cast<braids::MacroOscillatorShape>(engine_in));

  if (debug) {
    Serial.print(F("Oscillator shape set to engine: "));
    Serial.println(engine_in);
  }

  // initial reading of the pots with debounce
  readpot(0);
  readpot(1);
  readpot(2);


  int16_t timbre = (map(potvalue[0], POT_MIN, POT_MAX, 0, 32767));
  timbre_in = timbre;

  int16_t morph = (map(potvalue[1], POT_MIN, POT_MAX, 0, 32767));
  morph_in = morph;

  // fm / pitch updates
  // int16_t  pitch = map(potvalue[2], POT_MIN, POT_MAX, 16383, 0); // convert pitch CV data value to valid range
  // pitch_in = pitch - 1638;
  // used to switch between FM and note on cv3

  midiTimer = millis();

}



void loop() {

  if ( counter > 0 ) {
    updateBraidsAudio();
    counter = 0; // increments on each pass of the timer after the timer writes samples
  }

}

// second core dedicated to display foo

void setup1() {
  delay (200); // wait for main core to start up perhipherals
}




// second core deals with ui / control rate updates
void loop1() {

  //MIDI.read();
  uint32_t now = millis();
  // pot updates
  // reading A/D seems to cause noise in the audio so don't do it too often

  int16_t timbre = (map(potvalue[0], POT_MIN, POT_MAX, 0, 32767));
  timbre_in = timbre;
  int16_t morph = (map(potvalue[1], POT_MIN, POT_MAX, 0, 32767));
  morph_in = morph;

  // fm / pitch updates
  // int16_t  pitch = map(potvalue[2], POT_MIN, POT_MAX, 170, 0); // convert pitch CV data value to valid range
  // pitch_fm = pitch;

  int16_t pitch = map(potvalue[2], POT_MIN, POT_MAX, 3072, 8192); // convert pitch CV data value to valid range
  int16_t pitch_delta = abs(previous_pitch - pitch);
  if (pitch_delta > 10) {
    pitch_in = pitch;
    previous_pitch = pitch;
    //trigger_in = 1.0f; //retain for cv only input?
  }

  button.update();

  // A 10 s hold resets to the first engine and flashes the LED 5 times.
  // (The 500 ms "previous engine" step below will have fired once on the way
  // here; harmless, the reset overwrites it.)
  if (button.isPressed() && button.currentDuration() >= ENGINE_RESET_MS) {
    if (!resetHandled) {
      engineCount = DEFAULT_ENGINE;
      engine_in = engineCount;
      engineDirty = true;
      engineDirtyMs = now;
      resetHandled = true;
      longPressHandled = true;  // swallow the release so it doesn't step +1

      if (debug) {
        Serial.print(F("Engine RESET to: "));
        Serial.println(engineNames[engineCount]);
      }

      // Blocking is fine here: this core only does UI, and the audio pump
      // lives on core 0. The trigger-follow below repaints the LED afterwards.
      for (int i = 0; i < 5; i++) {
        digitalWrite(LED, HIGH);
        delay(100);
        digitalWrite(LED, LOW);
        delay(100);
      }
    }
  }
  // Check for long press (>500ms): decrement engine
  else if (button.isPressed() && button.currentDuration() > 500) {
    if (!longPressHandled) {
      engineCount--;
      if (engineCount < 0) {
        engineCount = 46;
      }
      engine_in = engineCount;
      engineDirty = true;
      engineDirtyMs = now;
      longPressHandled = true;

      if (debug) {
        Serial.print(F("Engine (long): "));
        Serial.print(engineCount);
        Serial.print(F(" - "));
        Serial.println(engineNames[engineCount]);
      }
    }
  } else if (button.released()) {
    // Short press: increment engine (only trigger on release if it wasn't a long press)
    if (!longPressHandled) {
      engineCount++;
      if (engineCount > 46) {
        engineCount = 0;
      }
      engine_in = engineCount;
      engineDirty = true;
      engineDirtyMs = now;

      if (debug) {
        Serial.print(F("Engine: "));
        Serial.print(engineCount);
        Serial.print(F(" - "));
        Serial.println(engineNames[engineCount]);
      }
    }
    longPressHandled = false;  // Reset flag when button is released
    resetHandled = false;      // next hold may reset again
  }

  // Debounced save: only after the engine has been still for a moment, and
  // never while the button is held (a 10 s hold would otherwise commit the
  // 500 ms "previous engine" step mid-hold). commit() itself parks the other
  // core (core 0, the audio pump) around the flash erase/program, so it stalls
  // the audio ISR for the few ms it takes — same audible cost as the other
  // MOD2 firmwares' saves. Do NOT wrap it in idleOtherCore()/resumeOtherCore():
  // commit() already idles the other core internally, and a second idle on the
  // same core re-enters the non-recursive SDK mutex and deadlocks, leaving the
  // audio core parked forever (silence until a power cycle).
  if (engineDirty && !button.isPressed() && (now - engineDirtyMs) > SAVE_DEBOUNCE_MS) {
    EEPROM.write(0, kSaveMagic0);
    EEPROM.write(1, kSaveMagic1);
    EEPROM.write(2, kSaveVersion);
    EEPROM.write(3, (uint8_t)engineCount);
    EEPROM.commit();
    engineDirty = false;

    if (debug) {
      Serial.print(F("Engine saved: "));
      Serial.println(engineNames[engineCount]);
    }
  }

  // read trigger in
  if (digitalRead(TRIG_PIN) ) {
    trigger_in = 1.0f;
    digitalWrite(LED, HIGH);  // Turn LED on when trigger received
  } else {
    trigger_in = 0.0f;
    digitalWrite(LED, LOW);   // Turn LED off when no trigger
  }
  // reading A/D seems to cause noise in the audio so don't do it too often

  if ((now - pot_timer) > POT_SAMPLE_TIME) {
    readpot(0);
    readpot(1);
    readpot(2);
    pot_timer = now;
  }






}
