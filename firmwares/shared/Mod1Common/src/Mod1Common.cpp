#include "Mod1Common.h"

namespace mod1 {

long mapClamp(long x, long in_min, long in_max, long out_min, long out_max) {
  if (in_max == in_min) {
    return out_min;
  }

  if (in_min < in_max) {
    if (x < in_min) x = in_min;
    if (x > in_max) x = in_max;
  } else {
    if (x > in_min) x = in_min;
    if (x < in_max) x = in_max;
  }

  return map(x, in_min, in_max, out_min, out_max);
}

unsigned long maxUL(unsigned long a, unsigned long b) {
  return (a > b) ? a : b;
}

int addClamp1023(int a, int b) {
  long sum = (long)a + (long)b;
  if (sum < 0) {
    return 0;
  }
  if (sum > 1023) {
    return 1023;
  }
  return (int)sum;
}

uint8_t select6FromAdc(int value) {
  if (value <= 102) {
    return 0;
  } else if (value <= 308) {
    return 1;
  } else if (value <= 514) {
    return 2;
  } else if (value <= 720) {
    return 3;
  } else if (value <= 926) {
    return 4;
  }
  return 5;
}

uint8_t select2FromAdc(int value) {
  if (value <= 511) {
    return 0;
  }
  return 1;
}

uint8_t select3FromAdc(int value) {
  if (value <= 340) {
    return 0;
  } else if (value <= 681) {
    return 1;
  }
  return 2;
}

uint8_t select4FromAdc(int value) {
  if (value <= 255) {
    return 0;
  } else if (value <= 511) {
    return 1;
  } else if (value <= 767) {
    return 2;
  }
  return 3;
}

void setupFastPwmEgStyle() {
#if defined(__AVR__) && defined(TCCR2A) && defined(TCCR2B) && defined(TCCR1A) && defined(TCCR1B) && defined(ICR1)
  TCCR2A = (1 << WGM21) | (1 << WGM20) | (1 << COM2B1);
  TCCR2B = (1 << CS20);
  TCCR1A = (1 << WGM11) | (1 << COM1A1);
  TCCR1B = (1 << WGM13) | (1 << WGM12) | (1 << CS10);
  ICR1 = 255;
#endif
}

void setupFastPwmLogicStyle() {
#if defined(__AVR__) && defined(TCCR2A) && defined(TCCR2B) && defined(TCCR1A) && defined(TCCR1B) && defined(OCR1B) && defined(OCR2A) && defined(OCR2B)
  TCCR1A = 0;
  TCCR1B = 0;
  TCCR1A = (1 << WGM10) | (1 << COM1B1) | (1 << COM1A1);
  TCCR1B = (1 << WGM12) | (1 << CS10);
  OCR1B = 0;

  TCCR2A = 0;
  TCCR2B = 0;
  TCCR2A = (1 << WGM20) | (1 << WGM21) | (1 << COM2A1) | (1 << COM2B1);
  TCCR2B = (1 << CS20);
  OCR2A = 0;
  OCR2B = 0;
#endif
}

DebouncedInput::DebouncedInput(unsigned long debounceMs, uint8_t initialState)
    : debounceMs_(debounceMs),
      lastChangeMs_(0),
      stableState_(initialState),
      lastReading_(initialState),
      rose_(false),
      fell_(false) {}

bool DebouncedInput::update(uint8_t reading, unsigned long nowMs) {
  rose_ = false;
  fell_ = false;

  if (reading != lastReading_) {
    lastReading_ = reading;
    lastChangeMs_ = nowMs;
  }

  if ((nowMs - lastChangeMs_) > debounceMs_ && stableState_ != lastReading_) {
    const uint8_t previous = stableState_;
    stableState_ = lastReading_;
    rose_ = (previous == LOW && stableState_ == HIGH);
    fell_ = (previous == HIGH && stableState_ == LOW);
    return true;
  }

  return false;
}

uint8_t DebouncedInput::state() const {
  return stableState_;
}

bool DebouncedInput::rose() const {
  return rose_;
}

bool DebouncedInput::fell() const {
  return fell_;
}

EdgeInput::EdgeInput(uint8_t initialState)
    : state_(initialState), rose_(false), fell_(false) {}

void EdgeInput::update(uint8_t reading) {
  rose_ = (state_ == LOW && reading == HIGH);
  fell_ = (state_ == HIGH && reading == LOW);
  state_ = reading;
}

bool EdgeInput::rose() const {
  return rose_;
}

bool EdgeInput::fell() const {
  return fell_;
}

uint8_t EdgeInput::state() const {
  return state_;
}

}  // namespace mod1
