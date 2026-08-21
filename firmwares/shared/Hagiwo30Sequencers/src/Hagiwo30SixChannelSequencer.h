#pragma once

// Six-channel drum sequencer engine for the HAGIWO #30 platform: pattern
// banks, fill-ins, per-channel mutes and EEPROM-backed step storage.
//
// License:
// MIT License, Copyright (c) 2026 Madelyn Yeary — see LICENSE.md in this
// library. Refactored out of HAGIWO's #30 sequencer firmware, released under
// CC0 1.0; CC0 places no conditions on derivative works.

#include <Encoder.h>
#include <Hagiwo30Common.h>
#include <Hagiwo30SequencerMode.h>
#include <Hagiwo30SixChDisplay.h>
#include <Hagiwo30SixChSequencerState.h>

class SixChannelSequencer : public hagiwo30::SequencerMode {
 public:
  SixChannelSequencer();
  void setup() override;
  void loop() override;

 private:
  void loadStepsFromEeprom();
  void saveStepsToEeprom();
  void applyProgramPattern(uint8_t sectionOffset, bool updateBankSelection);
  void setChannelGateOutput(uint8_t channel, bool high);
  void toggleSelectedStep();
  void toggleMuteForMenuBase(uint8_t menuBase);
  void handleManualButtonPress();
  void handleAutoButtonPress();
  void refreshDisplay();
  void saveData();
  void changeStep();
  void fillinStep();

  Encoder encoder_;
  hagiwo30::DebouncedActiveLowButton buttonDebounce_;
  hagiwo30::SixChSequencerState state_;
  hagiwo30::SixChDisplay sixChDisplay_;
  bool isInitialized_ = false;
};
