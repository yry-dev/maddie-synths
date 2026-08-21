#pragma once

// Display state struct and renderer for the Euclidean sequencer's OLED page.
//
// License:
// MIT License, Copyright (c) 2026 Madelyn Yeary — see LICENSE.md in this
// library. Refactored out of HAGIWO's #30 sequencer firmware, released under
// CC0 1.0; CC0 places no conditions on derivative works.

#include <Arduino.h>

#include <Hagiwo30Common.h>

namespace hagiwo30 {

struct EuclidDisplayState {
  const byte* hits;
  const byte* offset;
  const bool* mute;
  const byte* limit;
  const byte* playingStep;
  const byte (*patternTable)[16];
  uint8_t selectMenu;
  uint8_t selectCh;
  uint8_t barNow;
  const byte* barMax;
  uint8_t barSelect;
};

class EuclidDisplay {
 public:
  EuclidDisplay();

  bool begin();
  void render(const EuclidDisplayState& state);
};

}  // namespace hagiwo30
