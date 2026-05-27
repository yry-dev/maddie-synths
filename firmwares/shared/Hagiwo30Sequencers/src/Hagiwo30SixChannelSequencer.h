#pragma once

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
