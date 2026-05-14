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
