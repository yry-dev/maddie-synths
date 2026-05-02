#include "Hagiwo30Common.h"

#include <Wire.h>

namespace hagiwo30 {

namespace {

Adafruit_SSD1306 gSharedOled(kScreenWidth, kScreenHeight, &Wire, -1);
bool gSharedOledInitialized = false;

}  // namespace

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

Adafruit_SSD1306& sharedOledDisplay() {
  return gSharedOled;
}

bool beginSharedOledDisplay() {
  if (gSharedOledInitialized) {
    return true;
  }

  gSharedOledInitialized = gSharedOled.begin(SSD1306_SWITCHCAPVCC, kOledAddress);
  return gSharedOledInitialized;
}

}  // namespace hagiwo30
