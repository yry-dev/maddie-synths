#pragma once

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
