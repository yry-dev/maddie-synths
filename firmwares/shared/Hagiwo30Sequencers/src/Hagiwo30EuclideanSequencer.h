#pragma once

#include <Encoder.h>
#include <Hagiwo30Common.h>
#include <Hagiwo30EuclidDisplay.h>
#include <Hagiwo30EuclidSequencerState.h>
#include <Hagiwo30SequencerMode.h>

class EuclideanSequencer : public hagiwo30::SequencerMode {
 public:
  EuclideanSequencer();
  void setup() override;
  void loop() override;

 private:
  void setGateOutputByChannel(byte channel, bool high);
  void setAllGateOutputs(bool high);
  void refreshDisplay();
  void randomChange();

  Encoder encoder_;
  hagiwo30::DebouncedActiveLowButton buttonDebounce_;
  hagiwo30::EuclidDisplay euclidDisplay_;
  hagiwo30::EuclidSequencerState state_;
  bool isInitialized_ = false;
};
