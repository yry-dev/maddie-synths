#pragma once

// The setup()/loop() interface every #30 sequencer mode implements, plus the
// enum naming the available modes.
//
// License:
// MIT License, Copyright (c) 2026 Madelyn Yeary — see LICENSE.md in this
// library. Refactored out of HAGIWO's #30 sequencer firmware, released under
// CC0 1.0; CC0 places no conditions on derivative works.

namespace hagiwo30 {

enum class SequencerModeKind {
  SixChannel,
  Euclidean,
};

class SequencerMode {
 public:
  virtual ~SequencerMode() = default;
  virtual void setup() = 0;
  virtual void loop() = 0;
};

}  // namespace hagiwo30
