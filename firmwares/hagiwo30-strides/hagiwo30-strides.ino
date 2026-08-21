/* Strides

Description:
Dual-mode sequencer shell for the HAGIWO #30 platform: it hosts both the
six-channel step sequencer and the Euclidean sequencer and swaps between them
when the encoder button is held for 1.5 s. Only the active engine is ever
constructed — on a Nano the two cannot afford to co-exist in RAM — so switching
modes tears one down and builds the other. All sequencing lives in the shared
Hagiwo30Sequencers library; this sketch is the mode switch and nothing else.

License:
CC0 1.0 Universal (CC0 1.0) Public Domain Dedication
You can copy, modify, distribute and perform the work, even for commercial
purposes, all without asking permission.

Hardware:
HAGIWO #30 sequencer platform (Arduino Nano)
*/
#include <FastGPIO.h>
#include <Hagiwo30Common.h>
#include <Hagiwo30SequencerMode.h>

#include <Hagiwo30EuclideanSequencer.h>
#include <Hagiwo30SixChannelSequencer.h>

namespace {

constexpr unsigned long kModeSwitchHoldMs = 1500;
SixChannelSequencer* sixChannelMode = nullptr;
EuclideanSequencer* euclideanMode = nullptr;
hagiwo30::SequencerMode* activeMode = nullptr;
hagiwo30::SequencerModeKind activeModeKind = hagiwo30::SequencerModeKind::SixChannel;

hagiwo30::DebouncedActiveLowButton modeSwitchButton(60, HIGH);

unsigned long buttonPressStartMs = 0;
bool modeSwitchedForCurrentHold = false;

void constructActiveMode() {
  if (activeModeKind == hagiwo30::SequencerModeKind::SixChannel) {
    sixChannelMode = new SixChannelSequencer();
    activeMode = sixChannelMode;
  } else {
    euclideanMode = new EuclideanSequencer();
    activeMode = euclideanMode;
  }
}

void destroyActiveMode() {
  if (activeMode == nullptr) {
    return;
  }

  if (activeModeKind == hagiwo30::SequencerModeKind::SixChannel) {
    delete sixChannelMode;
    sixChannelMode = nullptr;
  } else {
    delete euclideanMode;
    euclideanMode = nullptr;
  }

  activeMode = nullptr;
}

void switchMode() {
  destroyActiveMode();
  activeModeKind = (activeModeKind == hagiwo30::SequencerModeKind::SixChannel)
                       ? hagiwo30::SequencerModeKind::Euclidean
                       : hagiwo30::SequencerModeKind::SixChannel;
  constructActiveMode();
  activeMode->setup();
}

void maybeSwitchModeOnLongHold() {
  const uint8_t buttonReading =
      FastGPIO::Pin<hagiwo30::kEncoderSwitchPin>::isInputHigh() ? HIGH : LOW;

  modeSwitchButton.update(buttonReading, millis());

  if (modeSwitchButton.fell()) {
    buttonPressStartMs = millis();
    modeSwitchedForCurrentHold = false;
  }

  const bool buttonHeldLow = (modeSwitchButton.state() == LOW);
  if (buttonHeldLow && !modeSwitchedForCurrentHold &&
      (millis() - buttonPressStartMs) >= kModeSwitchHoldMs) {
    switchMode();
    modeSwitchedForCurrentHold = true;
  }
}

}  // namespace

void setup() {
  constructActiveMode();
  activeMode->setup();
}

void loop() {
  maybeSwitchModeOnLongHold();
  activeMode->loop();
}
