#pragma once

// Display state struct and renderer for the six-channel sequencer's OLED
// page.
//
// License:
// MIT License, Copyright (c) 2026 Madelyn Yeary — see LICENSE.md in this
// library. Refactored out of HAGIWO's #30 sequencer firmware, released under
// CC0 1.0; CC0 places no conditions on derivative works.

#include <Arduino.h>

#include <Hagiwo30Common.h>

namespace hagiwo30 {

struct SixChDisplayState {
  uint16_t chStep[6];
  uint8_t chMute[6];
  uint8_t mode;
  uint8_t enc;
  uint8_t genre;
  uint8_t fillin;
  int repeatDone;
  uint8_t repeat;
  int repeatMax;
  int swDone;
  uint8_t sw;
  int swMax;
  uint8_t stepCount;
};

class SixChDisplay {
 public:
  SixChDisplay();

  bool begin();
  void render(const SixChDisplayState& state);

 private:
  void drawChannelPattern(uint8_t y, const char* label, uint16_t step, uint8_t mute);
  void drawManualSelection(uint8_t enc);
  void drawManualOptions(uint8_t enc);
  void drawAutoOptions(const SixChDisplayState& state);
  void drawChannelLabelSelection(uint8_t enc, uint8_t autoMode);
};

}  // namespace hagiwo30
