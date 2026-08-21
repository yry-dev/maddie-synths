#pragma once

// Holds the six-channel and Euclidean modes and forwards setup()/loop() to
// whichever is active.
//
// License:
// MIT License, Copyright (c) 2026 Madelyn Yeary — see LICENSE.md in this
// library. Refactored out of HAGIWO's #30 sequencer firmware, released under
// CC0 1.0; CC0 places no conditions on derivative works.

#include "Hagiwo30SequencerMode.h"

namespace hagiwo30 {

class SequencerModeManager {
 public:
  SequencerModeManager(SequencerMode& sixChannelMode, SequencerMode& euclideanMode);

  void setMode(SequencerModeKind mode, bool runSetup = true);
  SequencerModeKind mode() const;

  void setup();
  void loop();

 private:
  SequencerMode* activeMode();

  SequencerMode* sixChannelMode_;
  SequencerMode* euclideanMode_;
  SequencerModeKind currentMode_;
  bool isSetupComplete_;
};

}  // namespace hagiwo30
