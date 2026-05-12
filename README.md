# eurorack

My open source eurorack experiments.

## Tooling (macOS/linux + Homebrew)

- Install tools:
  - brew install arduino-cli python
- Optional GUI IDE:
  - brew install --cask arduino-ide

## Repo-local Arduino CLI config

This repo uses `arduino-cli.yaml` at the root.

It currently includes RP2040 board index support and works for both:

- Arduino Nano (official core)
- Raspberry Pi Pico (Earle Philhower core)

## One-time core install

- Arduino Nano / AVR:
  - arduino-cli core install arduino:avr --config-file ./arduino-cli.yaml
- Raspberry Pi Pico / RP2040:
  - arduino-cli core install rp2040:rp2040 --config-file ./arduino-cli.yaml

## One-time library install

Install external libraries used by the Testbild sketches:

- arduino-cli lib install "Encoder" "FastGPIO" "Adafruit SSD1306" "digitalWriteFast"

`Adafruit SSD1306` pulls `Adafruit GFX Library` and `Adafruit BusIO` automatically.

## Fish shell scripts

- Build:
  - scripts/build-fw.fish firmwares/mod1 arduino:avr:nano
- Upload:
  - scripts/upload-fw.fish firmwares/mod1 arduino:avr:nano /dev/cu.usbserial-XXXX

For Pico, use fqbn `rp2040:rp2040:rpipico`.

## Makefile builds

- Build every firmware into `dist/<firmware>/`:
  - make
- Build every firmware for a different board target:
  - make FQBN=rp2040:rp2040:rpipico
- Build a single firmware target:
  - make mod1-trigger-burst
- List discovered firmware targets:
  - make list
- Remove build output:
  - make clean

The Makefile discovers every sketch folder under `firmwares/` that contains a same-named `.ino` entry file and excludes `firmwares/shared/`.

## Shared library code

Use `firmwares/shared/` as the repo-local Arduino library root. Each library should use the standard Arduino layout:

- `firmwares/shared/<LibraryName>/library.properties`
- `firmwares/shared/<LibraryName>/src/<LibraryName>.h`
- `firmwares/shared/<LibraryName>/src/<LibraryName>.cpp`

This repo includes a starter library at `firmwares/shared/Mod1Common`.

Example use from a firmware sketch:

```cpp
#include <Mod1Common.h>

long cv = mod1::mapClamp(analogRead(A0), 0, 1023, 0, 255);
```

Both `make` and `scripts/build-fw.fish` automatically pass `firmwares/shared` as an Arduino CLI library search path.

### Mod1Common helpers

`firmwares/shared/Mod1Common` currently provides shared helpers used across the MOD1 sketches:

- Math and scaling:
  - `mod1::mapClamp(...)`
  - `mod1::addClamp1023(...)`
  - `mod1::select2FromAdc(...)`
  - `mod1::select3FromAdc(...)`
  - `mod1::select4FromAdc(...)`
  - `mod1::select6FromAdc(...)`
- Input utilities:
  - `mod1::DebouncedInput` for debounced button state and edge detection
  - `mod1::EdgeInput` for raw rising/falling edge detection
- AVR PWM setup (Arduino Nano / ATmega328P):
  - `mod1::setupFastPwmEgStyle()` for EG/LFO/Random-CV style outputs
  - `mod1::setupFastPwmLogicStyle()` for Logic module output mapping

PWM helpers are compile-guarded for AVR timer registers. On non-AVR targets they become no-ops.

Example:

```cpp
#include <Mod1Common.h>

void setup() {
  mod1::setupFastPwmEgStyle();
}

void loop() {
  int idx = mod1::select6FromAdc(analogRead(A0));
}
```

### TestbildCommon helpers

`firmwares/shared/TestbildCommon` provides shared constants/utilities used by the Testbild sequencer sketches:

- Board constants:
  - OLED address and dimensions
  - encoder/button/clock pins
  - 6 output channel pins
  - encoder detent count constants
- Input utility:
  - `testbild::DebouncedActiveLowButton` for active-low pushbutton debouncing

Example:

```cpp
#include <TestbildCommon.h>

Encoder enc(testbild::kEncoderPinA, testbild::kEncoderPinB);
testbild::DebouncedActiveLowButton button(300, HIGH);
```

## Adding many firmwares

Use one folder per firmware under `firmwares/`.

Important Arduino rule:

- The primary `.ino` file must match the folder name.

Example:

- `firmwares/mod1/mod1.ino` (entry sketch)
- Additional tabs/files can live beside it (for example `logic.ino`).

