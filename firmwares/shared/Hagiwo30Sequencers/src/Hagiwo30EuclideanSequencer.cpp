#include "Hagiwo30EuclideanSequencer.h"

#include <FastGPIO.h>
#include "Hagiwo30PatternBanks.h"

namespace {

constexpr byte kBarMax[4] = {2, 4, 8, 16};
constexpr byte kHitOccurrence[6] = {0, 10, 20, 20, 40, 80};
constexpr byte kOffsetOccurrence[6] = {10, 20, 20, 30, 40, 20};
constexpr byte kMuteOccurrence[6] = {20, 20, 20, 20, 20, 20};
constexpr byte kHitRangeMax[6] = {0, 14, 16, 8, 9, 16};
constexpr byte kHitRangeMin[6] = {0, 13, 6, 1, 5, 10};

}  // namespace

EuclideanSequencer::EuclideanSequencer()
    : encoder_(hagiwo30::kEncoderPinA, hagiwo30::kEncoderPinB),
      buttonDebounce_(300, HIGH) {}

void EuclideanSequencer::setup() {
  if (!isInitialized_) {
    euclidDisplay_.begin();
    FastGPIO::Pin<hagiwo30::kEncoderSwitchPin>::setInputPulledUp();
    FastGPIO::Pin<hagiwo30::kClockPin>::setInput();
    isInitialized_ = true;
  }

  refreshDisplay();
}

void EuclideanSequencer::loop() {
  state_.old_trg_in = state_.trg_in;
  state_.oldPosition = state_.newPosition;
  state_.newPosition = encoder_.read() / hagiwo30::kEncoderCountsPerClick;

  if (state_.newPosition < state_.oldPosition) {
    state_.oldPosition = state_.newPosition;
    state_.disp_refresh = 1;
    if (state_.select_menu != 0) {
      state_.select_menu--;
    }
  } else if (state_.newPosition > state_.oldPosition) {
    state_.oldPosition = state_.newPosition;
    state_.disp_refresh = 1;
    state_.select_menu++;
  }

  if (state_.select_ch != 6) {
    state_.select_menu = constrain(state_.select_menu, 0, 5);
  } else if (state_.select_ch == 6) {
    state_.select_menu = constrain(state_.select_menu, 0, 1);
  }

  bool buttonPressed = false;
  buttonDebounce_.update(FastGPIO::Pin<hagiwo30::kEncoderSwitchPin>::isInputHigh() ? HIGH : LOW, millis());
  if (buttonDebounce_.fell()) {
    buttonPressed = true;
    state_.disp_refresh = 1;
  }

  if (buttonPressed) {
    switch (state_.select_menu) {
      case 0:
        state_.select_ch++;
        if (state_.select_ch >= 7) {
          state_.select_ch = 0;
        }
        break;

      case 1:
        if (state_.select_ch != 6) {
          state_.hits[state_.select_ch]++;
          if (state_.hits[state_.select_ch] >= 17) {
            state_.hits[state_.select_ch] = 0;
          }
        } else if (state_.select_ch == 6) {
          state_.bar_select++;
          if (state_.bar_select >= 4) {
            state_.bar_select = 0;
          }
        }
        break;

      case 2:
        state_.offset[state_.select_ch]++;
        if (state_.offset[state_.select_ch] >= 16) {
          state_.offset[state_.select_ch] = 0;
        }
        break;

      case 3:
        state_.limit[state_.select_ch]++;
        if (state_.limit[state_.select_ch] >= 17) {
          state_.limit[state_.select_ch] = 0;
        }
        break;

      case 4:
        state_.mute[state_.select_ch] = !state_.mute[state_.select_ch];
        break;

      case 5:
        for (uint8_t ch = 0; ch <= 5; ch++) {
          state_.playing_step[ch] = 0;
        }
        break;
    }
  }

  auto shiftedPatternValue = [&](uint8_t channel, uint8_t step) -> uint8_t {
    return pgm_read_byte(&(euc16[state_.hits[channel]][(step + state_.offset[channel]) % 16]));
  };

  state_.trg_in = FastGPIO::Pin<hagiwo30::kClockPin>::isInputHigh();
  if (state_.old_trg_in == 0 && state_.trg_in == 1) {
    state_.gate_timer = millis();
    for (uint8_t ch = 0; ch <= 5; ch++) {
      state_.playing_step[ch]++;
      if (state_.playing_step[ch] >= state_.limit[ch]) {
        state_.playing_step[ch] = 0;
      }
    }

    for (uint8_t ch = 0; ch <= 5; ch++) {
      if (shiftedPatternValue(ch, state_.playing_step[ch]) == 1 && state_.mute[ch] == 0) {
        setGateOutputByChannel(ch, HIGH);
      }
    }

    if ((millis() - state_.last_refresh) > state_.max_refresh_time) {
      state_.disp_refresh = 1;
      state_.last_refresh = millis();
    }

    if (state_.select_ch == 6) {
      state_.step_cnt++;
      if (state_.step_cnt >= 16) {
        state_.bar_now++;
        state_.step_cnt = 0;
        if (state_.bar_now > kBarMax[state_.bar_select]) {
          state_.bar_now = 1;
          randomChange();
        }
      }
    }
  }

  if (state_.gate_timer + 100 <= millis()) {
    setAllGateOutputs(LOW);
  }

  if (state_.disp_refresh == 1) {
    refreshDisplay();
    state_.disp_refresh = 0;
  }
}

void EuclideanSequencer::setGateOutputByChannel(byte channel, bool high) {
  switch (channel) {
    case 0:
      FastGPIO::Pin<hagiwo30::kOutCh1>::setOutputValue(high);
      break;
    case 1:
      FastGPIO::Pin<hagiwo30::kOutCh2>::setOutputValue(high);
      break;
    case 2:
      FastGPIO::Pin<hagiwo30::kOutCh3>::setOutputValue(high);
      break;
    case 3:
      FastGPIO::Pin<hagiwo30::kOutCh4>::setOutputValue(high);
      break;
    case 4:
      FastGPIO::Pin<hagiwo30::kOutCh5>::setOutputValue(high);
      break;
    case 5:
      FastGPIO::Pin<hagiwo30::kOutCh6>::setOutputValue(high);
      break;
  }
}

void EuclideanSequencer::setAllGateOutputs(bool high) {
  for (byte ch = 0; ch <= 5; ch++) {
    setGateOutputByChannel(ch, high);
  }
}

void EuclideanSequencer::refreshDisplay() {
  hagiwo30::EuclidDisplayState displayState = {
      state_.hits,
      state_.offset,
      state_.mute,
      state_.limit,
      state_.playing_step,
      euc16,
      state_.select_menu,
      state_.select_ch,
      state_.bar_now,
      kBarMax,
      state_.bar_select};

  euclidDisplay_.render(displayState);
}

void EuclideanSequencer::randomChange() {
  for (uint8_t ch = 1; ch <= 5; ch++) {
    if (kHitOccurrence[ch] >= random(1, 100)) {
      state_.hits[ch] = random(kHitRangeMin[ch], kHitRangeMax[ch]);
    }

    if (kOffsetOccurrence[ch] >= random(1, 100)) {
      state_.offset[ch] = random(0, 16);
    }

    if (kMuteOccurrence[ch] >= random(1, 100)) {
      state_.mute[ch] = 1;
    } else if (kMuteOccurrence[ch] < random(1, 100)) {
      state_.mute[ch] = 0;
    }
  }
}
