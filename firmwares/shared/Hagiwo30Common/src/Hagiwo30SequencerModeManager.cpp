// Implementation of the sequencer mode manager — mode switching and
// setup()/loop() forwarding.
//
// License:
// MIT License, Copyright (c) 2026 Madelyn Yeary — see LICENSE.md in this
// library. Refactored out of HAGIWO's #30 sequencer firmware, released under
// CC0 1.0; CC0 places no conditions on derivative works.

#include "Hagiwo30SequencerModeManager.h"

namespace hagiwo30 {

SequencerModeManager::SequencerModeManager(SequencerMode& sixChannelMode, SequencerMode& euclideanMode)
    : sixChannelMode_(&sixChannelMode),
      euclideanMode_(&euclideanMode),
      currentMode_(SequencerModeKind::SixChannel),
      isSetupComplete_(false) {}

void SequencerModeManager::setMode(SequencerModeKind mode, bool runSetup) {
  currentMode_ = mode;

  if (runSetup && isSetupComplete_) {
    activeMode()->setup();
  }
}

SequencerModeKind SequencerModeManager::mode() const {
  return currentMode_;
}

void SequencerModeManager::setup() {
  activeMode()->setup();
  isSetupComplete_ = true;
}

void SequencerModeManager::loop() {
  activeMode()->loop();
}

SequencerMode* SequencerModeManager::activeMode() {
  if (currentMode_ == SequencerModeKind::Euclidean) {
    return euclideanMode_;
  }

  return sixChannelMode_;
}

}  // namespace hagiwo30
