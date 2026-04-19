#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include <TestbildCommon.h>

namespace testbild {

struct EuclidDisplayState {
  const byte* hits;
  const byte* offset;
  const bool* mute;
  const byte* limit;
  const byte* playingStep;
  uint8_t selectMenu;
  uint8_t selectCh;
  uint8_t barNow;
  const byte* barMax;
  uint8_t barSelect;
  const bool (*offsetBuf)[16];
};

class EuclidDisplay {
 public:
  EuclidDisplay();

  bool begin();
  void render(const EuclidDisplayState& state);

 private:
  Adafruit_SSD1306 display_;
};

}  // namespace testbild
