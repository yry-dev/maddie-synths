#pragma once

#include <Arduino.h>

namespace testbild {

constexpr uint8_t kOledAddress = 0x3C;
constexpr uint8_t kScreenWidth = 128;
constexpr uint8_t kScreenHeight = 64;

constexpr uint8_t kEncoderPinA = 2;
constexpr uint8_t kEncoderPinB = 3;
constexpr uint8_t kEncoderSwitchPin = 12;
constexpr uint8_t kClockPin = 13;

constexpr uint8_t kOutCh1 = 5;
constexpr uint8_t kOutCh2 = 6;
constexpr uint8_t kOutCh3 = 7;
constexpr uint8_t kOutCh4 = 8;
constexpr uint8_t kOutCh5 = 9;
constexpr uint8_t kOutCh6 = 10;

constexpr uint8_t kEncoderCountsPerRotation = 4;
constexpr uint8_t kEncoderCountsPerClick = 4;

class DebouncedActiveLowButton {
 public:
  DebouncedActiveLowButton(unsigned long debounceMs, uint8_t initialState = HIGH);

  // Update with raw pin reading. Returns true when stable state changes.
  bool update(uint8_t reading, unsigned long nowMs);
  bool fell() const;
  bool rose() const;
  uint8_t state() const;

 private:
  unsigned long debounceMs_;
  unsigned long lastChangeMs_;
  uint8_t stableState_;
  uint8_t lastReading_;
  bool fell_;
  bool rose_;
};

}  // namespace testbild
