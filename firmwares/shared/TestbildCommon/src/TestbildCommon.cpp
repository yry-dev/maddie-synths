#include "TestbildCommon.h"

namespace testbild {

DebouncedActiveLowButton::DebouncedActiveLowButton(unsigned long debounceMs,
                                                   uint8_t initialState)
    : debounceMs_(debounceMs),
      lastChangeMs_(0),
      stableState_(initialState),
      lastReading_(initialState),
      fell_(false),
      rose_(false) {}

bool DebouncedActiveLowButton::update(uint8_t reading, unsigned long nowMs) {
  fell_ = false;
  rose_ = false;

  if (reading != lastReading_) {
    lastReading_ = reading;
    lastChangeMs_ = nowMs;
  }

  if ((nowMs - lastChangeMs_) > debounceMs_ && stableState_ != lastReading_) {
    const uint8_t previous = stableState_;
    stableState_ = lastReading_;
    fell_ = (previous == HIGH && stableState_ == LOW);
    rose_ = (previous == LOW && stableState_ == HIGH);
    return true;
  }

  return false;
}

bool DebouncedActiveLowButton::fell() const {
  return fell_;
}

bool DebouncedActiveLowButton::rose() const {
  return rose_;
}

uint8_t DebouncedActiveLowButton::state() const {
  return stableState_;
}

}  // namespace testbild
