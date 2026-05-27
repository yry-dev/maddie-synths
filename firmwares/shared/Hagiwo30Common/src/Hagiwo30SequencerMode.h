#pragma once

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
