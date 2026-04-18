#pragma once

#include <Arduino.h>

namespace mod1 {

// MOD1 panel pin mapping shared by all firmware variants.
constexpr uint8_t PIN_POT1 = A0;
constexpr uint8_t PIN_POT2 = A1;
constexpr uint8_t PIN_POT3 = A2;
constexpr uint8_t PIN_CV1 = A3;
constexpr uint8_t PIN_CV2 = A4;
constexpr uint8_t PIN_CV3 = A5;
constexpr uint8_t PIN_F1 = 17;
constexpr uint8_t PIN_F2 = 9;
constexpr uint8_t PIN_F3 = 10;
constexpr uint8_t PIN_F4 = 11;
constexpr uint8_t PIN_BUTTON = 4;
constexpr uint8_t PIN_LED = 3;

// Map while clamping the input into the source range.
long mapClamp(long x, long in_min, long in_max, long out_min, long out_max);

// Return max of a and b for unsigned long values.
unsigned long maxUL(unsigned long a, unsigned long b);

// Add two ADC values and clamp to the Arduino ADC range.
int addClamp1023(int a, int b);

// Convert an ADC value (0..1023) to a 6-position selector index (0..5).
uint8_t select6FromAdc(int value);

// Convert an ADC value (0..1023) to a 2-position selector index (0..1).
uint8_t select2FromAdc(int value);

// Convert an ADC value (0..1023) to a 3-position selector index (0..2).
uint8_t select3FromAdc(int value);

// Convert an ADC value (0..1023) to a 4-position selector index (0..3).
uint8_t select4FromAdc(int value);

// Configure Timer1/Timer2 for ~62.5kHz PWM used by EG/LFO/Random-CV sketches.
void setupFastPwmEgStyle();

// Configure Timer1/Timer2 for ~62.5kHz PWM used by Logic sketch.
void setupFastPwmLogicStyle();

class DebouncedInput {
 public:
	DebouncedInput(unsigned long debounceMs, uint8_t initialState = HIGH);

	// Update with raw input. Returns true when stable state changes.
	bool update(uint8_t reading, unsigned long nowMs);
	uint8_t state() const;
	bool rose() const;
	bool fell() const;

 private:
	unsigned long debounceMs_;
	unsigned long lastChangeMs_;
	uint8_t stableState_;
	uint8_t lastReading_;
	bool rose_;
	bool fell_;
};

class EdgeInput {
 public:
	EdgeInput(uint8_t initialState = LOW);

	void update(uint8_t reading);
	bool rose() const;
	bool fell() const;
	uint8_t state() const;

 private:
	uint8_t state_;
	bool rose_;
	bool fell_;
};

}  // namespace mod1
