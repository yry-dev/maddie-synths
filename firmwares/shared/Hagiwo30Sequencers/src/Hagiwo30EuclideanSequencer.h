#pragma once

// Euclidean sequencer engine for the HAGIWO #30 platform: six channels of
// Euclidean patterns driven by the encoder + OLED UI.
//
// License:
// MIT License, Copyright (c) 2026 Madelyn Yeary — see LICENSE.md in this
// library. Refactored out of HAGIWO's #30 sequencer firmware, released under
// CC0 1.0; CC0 places no conditions on derivative works.

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
