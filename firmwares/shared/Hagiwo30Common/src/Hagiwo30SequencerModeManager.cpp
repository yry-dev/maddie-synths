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
