#include "Hagiwo30EuclidDisplay.h"

#include <Adafruit_GFX.h>

namespace hagiwo30 {

EuclidDisplay::EuclidDisplay() = default;

bool EuclidDisplay::begin() {
  return beginSharedOledDisplay();
}

void EuclidDisplay::render(const EuclidDisplayState& state) {
  Adafruit_SSD1306& display = sharedOledDisplay();

  const uint8_t graphX[6] = {0, 40, 80, 15, 55, 95};
  const uint8_t graphY[6] = {0, 0, 0, 32, 32, 32};
  const uint8_t x16[16] = {15, 21, 26, 29, 30, 29, 26, 21, 15, 9, 4, 1, 0, 1, 4, 9};
  const uint8_t y16[16] = {0, 1, 4, 9, 15, 21, 26, 29, 30, 29, 26, 21, 15, 9, 4, 1};

  uint8_t lineXBuf[17] = {0};
  uint8_t lineYBuf[17] = {0};

  auto shiftedPatternValue = [&](uint8_t channel, uint8_t step) -> uint8_t {
    return pgm_read_byte(&(state.patternTable[state.hits[channel]][(step + state.offset[channel]) % 16]));
  };

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);

  display.setCursor(120, 0);
  if (state.selectCh != 6) {
    display.print(state.selectCh + 1);
  } else {
    display.print("R");
  }

  display.setCursor(120, 9);
  if (state.selectCh != 6) {
    display.print("H");
  } else {
    display.print("O");
  }

  display.setCursor(120, 18);
  if (state.selectCh != 6) {
    display.print("O");
    display.setCursor(0, 36);
    display.print("L");
    display.setCursor(0, 45);
    display.print("M");
    display.setCursor(0, 54);
    display.print("R");
  }

  if (state.selectCh == 6) {
    display.drawRect(1, 62 - state.barMax[state.barSelect] * 2, 6, state.barMax[state.barSelect] * 2 + 2, WHITE);
    display.fillRect(1, 64 - state.barNow * 2, 6, state.barMax[state.barSelect] * 2, WHITE);
  }

  if (state.selectMenu == 0) {
    display.drawTriangle(113, 0, 113, 6, 118, 3, WHITE);
  } else if (state.selectMenu == 1) {
    display.drawTriangle(113, 9, 113, 15, 118, 12, WHITE);
  }

  if (state.selectCh != 6) {
    if (state.selectMenu == 2) {
      display.drawTriangle(113, 18, 113, 24, 118, 21, WHITE);
    } else if (state.selectMenu == 3) {
      display.drawTriangle(12, 36, 12, 42, 7, 39, WHITE);
    } else if (state.selectMenu == 4) {
      display.drawTriangle(12, 45, 12, 51, 7, 48, WHITE);
    } else if (state.selectMenu == 5) {
      display.drawTriangle(12, 54, 12, 60, 7, 57, WHITE);
    }
  }

  for (uint8_t k = 0; k <= 5; k++) {
    for (uint8_t j = 0; j <= state.limit[k] - 1; j++) {
      display.drawPixel(x16[j] + graphX[k], y16[j] + graphY[k], WHITE);
    }
  }

  for (uint8_t k = 0; k <= 5; k++) {
    uint8_t bufCount = 0;
    for (uint8_t m = 0; m < 16; m++) {
      if (shiftedPatternValue(k, m) == 1) {
        lineXBuf[bufCount] = x16[m] + graphX[k];
        lineYBuf[bufCount] = y16[m] + graphY[k];
        bufCount++;
      }
    }

    if (bufCount >= 2) {
      for (uint8_t j = 0; j < bufCount - 1; j++) {
        display.drawLine(lineXBuf[j], lineYBuf[j], lineXBuf[j + 1], lineYBuf[j + 1], WHITE);
      }
      display.drawLine(lineXBuf[0], lineYBuf[0], lineXBuf[bufCount - 1], lineYBuf[bufCount - 1], WHITE);
    }
  }

  for (uint8_t k = 0; k <= 5; k++) {
    if (state.hits[k] == 1) {
      display.drawLine(15 + graphX[k], 15 + graphY[k], x16[state.offset[k]] + graphX[k], y16[state.offset[k]] + graphY[k], WHITE);
    }
  }

  for (uint8_t k = 0; k <= 5; k++) {
    if (state.mute[k] == 0) {
      if (shiftedPatternValue(k, state.playingStep[k]) == 0) {
        display.drawCircle(x16[state.playingStep[k]] + graphX[k], y16[state.playingStep[k]] + graphY[k], 2, WHITE);
      }
      if (shiftedPatternValue(k, state.playingStep[k]) == 1) {
        display.fillCircle(x16[state.playingStep[k]] + graphX[k], y16[state.playingStep[k]] + graphY[k], 3, WHITE);
      }
    }
  }

  display.display();
}

}  // namespace hagiwo30
