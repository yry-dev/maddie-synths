#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include <TestbildCommon.h>

namespace testbild {

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

  Adafruit_SSD1306 display_;
};

}  // namespace testbild
