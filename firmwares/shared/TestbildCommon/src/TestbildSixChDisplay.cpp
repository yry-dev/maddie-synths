#include "TestbildSixChDisplay.h"

namespace testbild {

SixChDisplay::SixChDisplay()
    : display_(kScreenWidth, kScreenHeight, &Wire, -1) {}

bool SixChDisplay::begin() {
  return display_.begin(SSD1306_SWITCHCAPVCC, kOledAddress);
}

void SixChDisplay::drawChannelPattern(uint8_t y, const char* label, uint16_t step, uint8_t mute) {
  display_.setCursor(0, y);
  display_.setTextColor(WHITE);
  display_.print(label);

  display_.setCursor(30, y);
  if (mute == 0) {
    for (int bit = 15; bit >= 0; --bit) {
      display_.print(bitRead(step, bit) ? "*" : "_");
    }
  }
}

void SixChDisplay::drawManualSelection(uint8_t enc) {
  if (enc <= 16) {
    display_.drawRect(enc * 6 + 24, 0, 6, 8, WHITE);
  } else if (enc <= 32) {
    display_.drawRect((enc - 16) * 6 + 24, 9, 6, 8, WHITE);
  } else if (enc <= 48) {
    display_.drawRect((enc - 32) * 6 + 24, 18, 6, 8, WHITE);
  } else if (enc <= 64) {
    display_.drawRect((enc - 48) * 6 + 24, 27, 6, 8, WHITE);
  } else if (enc <= 80) {
    display_.drawRect((enc - 64) * 6 + 24, 36, 6, 8, WHITE);
  } else if (enc <= 96) {
    display_.drawRect((enc - 80) * 6 + 24, 45, 6, 8, WHITE);
  }
}

void SixChDisplay::drawManualOptions(uint8_t enc) {
  display_.setCursor(0, 54);
  display_.setTextColor(enc == 97 ? BLACK : WHITE, enc == 97 ? WHITE : BLACK);
  display_.print("MNAL");

  display_.setCursor(48, 54);
  display_.setTextColor(enc == 98 ? BLACK : WHITE, enc == 98 ? WHITE : BLACK);
  display_.print("RESET");

  display_.setCursor(102, 54);
  display_.setTextColor(enc == 99 ? BLACK : WHITE, enc == 99 ? WHITE : BLACK);
  display_.print("SAVE");
}

void SixChDisplay::drawChannelLabelSelection(uint8_t enc, uint8_t autoMode) {
  const uint8_t base = autoMode ? 6 : 100;
  const uint8_t y[6] = {0, 9, 18, 27, 36, 45};
  const char* labels[6] = {"CH1", "CH2", "CH3", "CH4", "CH5", "CH6"};

  for (uint8_t i = 0; i < 6; ++i) {
    const uint8_t selected = enc == (base + i);
    display_.setTextColor(selected ? BLACK : WHITE, selected ? WHITE : BLACK);
    display_.setCursor(0, y[i]);
    display_.print(labels[i]);
  }
}

void SixChDisplay::drawAutoOptions(const SixChDisplayState& state) {
  display_.setCursor(0, 54);

  if (state.enc <= 3) {
    display_.setTextColor(state.enc == 1 ? BLACK : WHITE, state.enc == 1 ? WHITE : BLACK);
    display_.print("AUTO");

    display_.setTextColor(WHITE);
    display_.print("  ");

    display_.setTextColor(state.enc == 2 ? BLACK : WHITE, state.enc == 2 ? WHITE : BLACK);
    switch (state.genre) {
      case 0:
        display_.print("TECHNO");
        break;
      case 1:
        display_.print("DUBTCN");
        break;
      default:
        display_.print("HOUSE ");
        break;
    }

    display_.setTextColor(WHITE);
    display_.print("  ");

    display_.setTextColor(state.enc == 3 ? BLACK : WHITE, state.enc == 3 ? WHITE : BLACK);
    display_.print(state.fillin == 0 ? "FilIN:N" : "FilIN:Y");
    return;
  }

  display_.setCursor(0, 54);
  display_.setTextColor(state.enc == 4 ? BLACK : WHITE, state.enc == 4 ? WHITE : BLACK);
  display_.print("REP:");
  display_.print(state.repeatDone + 1);
  display_.print("/");
  if (state.repeat <= 3) {
    display_.print(state.repeatMax);
  } else {
    display_.print("ET");
  }

  display_.setCursor(70, 54);
  display_.setTextColor(state.enc == 5 ? BLACK : WHITE, state.enc == 5 ? WHITE : BLACK);
  display_.print("SW:");
  display_.print(state.swDone + 1);
  display_.print("/");
  if (state.sw <= 3) {
    display_.print(state.swMax);
  } else {
    display_.print("ET");
  }

  drawChannelLabelSelection(state.enc, 1);
}

void SixChDisplay::render(const SixChDisplayState& state) {
  display_.clearDisplay();
  display_.setTextSize(1);
  display_.setTextColor(WHITE);

  drawChannelPattern(0, "CH1", state.chStep[0], state.chMute[0]);
  drawChannelPattern(9, "CH2", state.chStep[1], state.chMute[1]);
  drawChannelPattern(18, "CH3", state.chStep[2], state.chMute[2]);
  drawChannelPattern(27, "CH4", state.chStep[3], state.chMute[3]);
  drawChannelPattern(36, "CH5", state.chStep[4], state.chMute[4]);
  drawChannelPattern(45, "CH6", state.chStep[5], state.chMute[5]);

  if (state.mode == 0) {
    drawManualSelection(state.enc);
    drawManualOptions(state.enc);
    drawChannelLabelSelection(state.enc, 0);
  } else {
    drawAutoOptions(state);
  }

  display_.drawRect(state.stepCount * 6 + 24, 0, 6, 53, WHITE);
  display_.setCursor(0, 54);
  display_.display();
}

}  // namespace testbild
