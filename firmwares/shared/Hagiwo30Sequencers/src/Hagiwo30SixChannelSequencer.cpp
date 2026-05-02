#include "Hagiwo30SixChannelSequencer.h"

#include <EEPROM.h>
#include <FastGPIO.h>
#include "Hagiwo30ProgramBanks.h"

SixChannelSequencer::SixChannelSequencer()
    : encoder_(hagiwo30::kEncoderPinA, hagiwo30::kEncoderPinB),
      buttonDebounce_(300, HIGH) {}

void SixChannelSequencer::setup() {
  if (!isInitialized_) {
    sixChDisplay_.begin();
    FastGPIO::Pin<hagiwo30::kEncoderSwitchPin>::setInputPulledUp();
    FastGPIO::Pin<hagiwo30::kClockPin>::setInput();
    loadStepsFromEeprom();
    isInitialized_ = true;
  }

  refreshDisplay();
}

void SixChannelSequencer::loop() {
  state_.old_clock_in = state_.clock_in;
  state_.refresh_display = false;

  if (state_.mode == 0) {
    state_.enc_max = 105;
  } else if (state_.mode == 1) {
    state_.enc_max = 11;
  }

  state_.newPosition = encoder_.read() / hagiwo30::kEncoderCountsPerRotation;

  if (state_.newPosition < state_.oldPosition) {
    state_.oldPosition = state_.newPosition;
    state_.enc = state_.enc - 1;
    state_.refresh_display = true;
  } else if (state_.newPosition > state_.oldPosition) {
    state_.oldPosition = state_.newPosition;
    state_.enc = state_.enc + 1;
    state_.refresh_display = true;
  }

  if (state_.enc <= 0) {
    state_.enc = state_.enc_max;
  } else if (state_.enc > state_.enc_max) {
    state_.enc = 1;
  }

  if (state_.mode == 0) {
    state_.enc_bit = 0;
    bitSet(state_.enc_bit, abs(state_.enc % 16 - 16));
    if (abs(state_.enc % 16 - 16) == 16) {
      bitSet(state_.enc_bit, 0);
    }
  }

  buttonDebounce_.update(FastGPIO::Pin<hagiwo30::kEncoderSwitchPin>::isInputHigh() ? HIGH : LOW, millis());
  if (buttonDebounce_.fell()) {
    state_.button_on = 1;
    state_.refresh_display = true;
  } else {
    state_.button_on = 0;
  }

  if (state_.mode == 0) {
    if (state_.button_on == 1) {
      handleManualButtonPress();
    }
  } else if (state_.mode == 1) {
    if (state_.button_on == 1) {
      handleAutoButtonPress();
    }
  }

  switch (state_.repeat) {
    case 0:
      state_.repeat_max = 4;
      break;
    case 1:
      state_.repeat_max = 8;
      break;
    case 2:
      state_.repeat_max = 16;
      break;
    case 3:
      state_.repeat_max = 32;
      break;
    case 4:
      state_.repeat_max = 10000;
      break;
  }

  switch (state_.sw) {
    case 0:
      state_.sw_max = 2;
      break;
    case 1:
      state_.sw_max = 4;
      break;
    case 2:
      state_.sw_max = 8;
      break;
    case 3:
      state_.sw_max = 16;
      break;
    case 4:
      state_.sw_max = 255;
      break;
  }

  state_.clock_in = FastGPIO::Pin<hagiwo30::kClockPin>::isInputHigh();

  if (state_.old_clock_in == 0 && state_.clock_in == 1) {
    if ((millis() - state_.last_refresh) > state_.max_refresh_time) {
      state_.refresh_display = true;
      state_.last_refresh = millis();
    }
    state_.step_count++;
  }

  if (state_.step_count >= 17) {
    state_.step_count = 1;

    if (state_.mode == 1) {
      state_.repeat_done++;

      if (state_.fillin == 1 && state_.repeat_done == state_.repeat_max - 1) {
        fillinStep();
      } else if (state_.repeat_done >= state_.repeat_max) {
        state_.sw_done++;
        state_.repeat_done = 0;
        changeStep();
      }
    }
  }

  if (state_.sw_done >= state_.sw_max) {
    state_.sw_done = 0;
  }

  unsigned int* steps[6] = {
      &state_.ch1_step,
      &state_.ch2_step,
      &state_.ch3_step,
      &state_.ch4_step,
      &state_.ch5_step,
      &state_.ch6_step};
  byte* outputs[6] = {
      &state_.CH1_output,
      &state_.CH2_output,
      &state_.CH3_output,
      &state_.CH4_output,
      &state_.CH5_output,
      &state_.CH6_output};
  byte* mutes[6] = {
      &state_.CH1_mute,
      &state_.CH2_mute,
      &state_.CH3_mute,
      &state_.CH4_mute,
      &state_.CH5_mute,
      &state_.CH6_mute};

  for (uint8_t ch = 0; ch < 6; ++ch) {
    *outputs[ch] = bitRead(*steps[ch], 16 - state_.step_count);
    setChannelGateOutput(ch, state_.clock_in && *outputs[ch] && !*mutes[ch]);
  }

  if ((state_.old_clock_in == 0 && state_.clock_in == 1) || state_.refresh_display) {
    refreshDisplay();
  }
}

void SixChannelSequencer::loadStepsFromEeprom() {
  unsigned int* steps[6] = {
      &state_.ch1_step,
      &state_.ch2_step,
      &state_.ch3_step,
      &state_.ch4_step,
      &state_.ch5_step,
      &state_.ch6_step};

  for (uint8_t ch = 0; ch < 6; ++ch) {
    const uint8_t base = 1 + (ch * 2);
    *steps[ch] = (static_cast<uint16_t>(EEPROM.read(base)) << 8) | EEPROM.read(base + 1);
  }
}

void SixChannelSequencer::saveStepsToEeprom() {
  const unsigned int steps[6] = {
      state_.ch1_step,
      state_.ch2_step,
      state_.ch3_step,
      state_.ch4_step,
      state_.ch5_step,
      state_.ch6_step};

  for (uint8_t ch = 0; ch < 6; ++ch) {
    const uint8_t base = 1 + (ch * 2);
    EEPROM.update(base, highByte(steps[ch]));
    EEPROM.update(base + 1, lowByte(steps[ch]));
  }
}

void SixChannelSequencer::applyProgramPattern(uint8_t sectionOffset, bool updateBankSelection) {
  const unsigned int (*bank)[12] = nullptr;
  byte* bankIndex = nullptr;
  uint8_t randomMax = 0;

  switch (state_.genre) {
    case 0:
      bank = bnk1_ptn;
      bankIndex = &state_.change_bnk1;
      randomMax = 7;
      break;
    case 1:
      bank = bnk2_ptn;
      bankIndex = &state_.change_bnk2;
      randomMax = 4;
      break;
    case 2:
      bank = bnk3_ptn;
      bankIndex = &state_.change_bnk3;
      randomMax = 4;
      break;
    default:
      return;
  }

  if (updateBankSelection && state_.sw_done >= state_.sw_max) {
    *bankIndex = random(0, randomMax);
  }

  unsigned int* steps[6] = {
      &state_.ch1_step,
      &state_.ch2_step,
      &state_.ch3_step,
      &state_.ch4_step,
      &state_.ch5_step,
      &state_.ch6_step};

  for (uint8_t ch = 0; ch < 6; ++ch) {
    *steps[ch] = pgm_read_word(&(bank[*bankIndex][sectionOffset + ch]));
  }
}

void SixChannelSequencer::setChannelGateOutput(uint8_t channel, bool high) {
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

void SixChannelSequencer::toggleSelectedStep() {
  unsigned int* steps[6] = {
      &state_.ch1_step,
      &state_.ch2_step,
      &state_.ch3_step,
      &state_.ch4_step,
      &state_.ch5_step,
      &state_.ch6_step};

  const uint8_t channel = (state_.enc - 1) / 16;
  if (channel < 6) {
    *steps[channel] ^= state_.enc_bit;
  }
}

void SixChannelSequencer::toggleMuteForMenuBase(uint8_t menuBase) {
  byte* mutes[6] = {
      &state_.CH1_mute,
      &state_.CH2_mute,
      &state_.CH3_mute,
      &state_.CH4_mute,
      &state_.CH5_mute,
      &state_.CH6_mute};

  const uint8_t channel = state_.enc - menuBase;
  if (channel < 6) {
    *mutes[channel] = !*mutes[channel];
  }
}

void SixChannelSequencer::handleManualButtonPress() {
  if (state_.enc <= 96) {
    toggleSelectedStep();
  } else if (state_.enc == 97) {
    state_.mode = 1;
    changeStep();
  } else if (state_.enc == 98) {
    state_.step_count = 1;
  } else if (state_.enc == 99) {
    saveData();
    state_.step_count = 1;
  } else if (state_.enc >= 100 && state_.enc <= 105) {
    toggleMuteForMenuBase(100);
  }
}

void SixChannelSequencer::handleAutoButtonPress() {
  if (state_.enc == 1) {
    state_.mode = 0;
    state_.enc = 97;
    loadStepsFromEeprom();
  } else if (state_.enc == 2) {
    state_.genre++;
    if (state_.genre >= 3) {
      state_.genre = 0;
    }
  } else if (state_.enc == 3) {
    state_.fillin = !state_.fillin;
  } else if (state_.enc == 4) {
    state_.repeat++;
    if (state_.repeat >= 5) {
      state_.repeat = 0;
    }
  } else if (state_.enc == 5) {
    state_.sw++;
    if (state_.sw >= 5) {
      state_.sw = 0;
    }
  } else if (state_.enc >= 6 && state_.enc <= 11) {
    toggleMuteForMenuBase(6);
  }
}

void SixChannelSequencer::refreshDisplay() {
  hagiwo30::SixChDisplayState displayState = {
      {state_.ch1_step, state_.ch2_step, state_.ch3_step, state_.ch4_step, state_.ch5_step, state_.ch6_step},
      {state_.CH1_mute, state_.CH2_mute, state_.CH3_mute, state_.CH4_mute, state_.CH5_mute, state_.CH6_mute},
      state_.mode,
      state_.enc,
      state_.genre,
      state_.fillin,
      state_.repeat_done,
      state_.repeat,
      state_.repeat_max,
      state_.sw_done,
      state_.sw,
      state_.sw_max,
      state_.step_count};

  sixChDisplay_.render(displayState);
}

void SixChannelSequencer::saveData() {
  saveStepsToEeprom();
}

void SixChannelSequencer::changeStep() {
  applyProgramPattern(0, true);
}

void SixChannelSequencer::fillinStep() {
  applyProgramPattern(6, false);
}
