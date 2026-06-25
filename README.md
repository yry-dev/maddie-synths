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

### MOD2 Braids / Tides (Mutable Instruments port)

`mod2-braids` and `mod2-tides` depend on Mutable Instruments DSP code and two
helper libraries that are **not** vendored in this repo. Install them once into
your Arduino libraries folder (default `~/Documents/Arduino/libraries`).

1. Library Manager dependencies:

   - arduino-cli lib install "Bounce2" "RPI_PICO_TimerInterrupt"

2. Mutable Instruments libraries from [poetaster/arduinoMI](https://github.com/poetaster/arduinoMI)
   (the modules live in separate submodule repos — clone the three we need over
   HTTPS straight into the libraries folder):

   - `git clone https://github.com/poetaster/STMLIB.git ~/Documents/Arduino/libraries/STMLIB`
   - `git clone https://github.com/poetaster/BRAIDS.git ~/Documents/Arduino/libraries/BRAIDS`
   - `git clone https://github.com/poetaster/TIDES.git  ~/Documents/Arduino/libraries/TIDES`

3. **TIDES packaging fix.** The `TIDES` library ships its sub-sources as `.cc`
   files *and* amalgamates them in `src/tides_all.cpp`, so arduino-cli compiles
   them twice and the link fails with `multiple definition` errors. `BRAIDS`
   already uses `.inc` for the same trick; make `TIDES` match by renaming its
   three sub-sources and updating the includes:

   ```sh
   cd ~/Documents/Arduino/libraries/TIDES/src
   for f in resources poly_slope_generator ramp_extractor; do
     mv "tides2/$f.cc" "tides2/$f.inc"
   done
   sed -i '' -E 's#(tides2/[a-z_]+)\.cc#\1.inc#' tides_all.cpp
   ```

`PWMAudio.h` (used by both) ships with the `rp2040:rp2040` core, so no extra
install is needed for it. After this, `make mod2-braids` and `make mod2-tides`
build cleanly.

## Fish shell scripts

- Build:
  - scripts/build-fw.fish firmwares/mod1 arduino:avr:nano
- Upload:
  - scripts/upload-fw.fish mod1-triple-wave-lfo arduino:avr:nano /dev/cu.usbserial-XXXX

`scripts/upload-fw.fish` uploads prebuilt binaries from `dist/<firmware>/`.
It accepts either a firmware name (for example `mod1-triple-wave-lfo`) or a sketch path under `firmwares/`.

For Pico, use fqbn `rp2040:rp2040:rpipico`.

## Makefile firmware builds

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

### Mod2Common helpers

`firmwares/shared/Mod2Common` provides shared scaffolding used across the MOD2 (Seeed Xiao RP2350) sketches that share the ~36.6 kHz dual-slice PWM audio path:

- Panel pin map (note the `*_PIN` suffix — the RP2350 core already defines a `PIN_LED` macro):
  - `mod2::POT1_PIN` / `POT2_PIN` / `POT3_PIN`, `mod2::CV_PIN`
  - `mod2::IN1_PIN`, `mod2::IN2_PIN`, `mod2::OUT_PIN`, `mod2::LED_PIN`, `mod2::BUTTON_PIN`
- Audio path:
  - `mod2::initAudioPwm(audioSlice, timerSlice, handler)` — GPIO1 10-bit audio + GPIO2 ~36.6 kHz wrap-IRQ
  - `mod2::initPwmOutput10bit(pin)` — 10-bit PWM output (e.g. LED brightness)
  - constants `SYS_CLOCK`, `AUDIO_FS`, `PWM_AUDIO_WRAP`, `PWM_TIMER_WRAP`, `PWM_FS`, `PWM_MID`
- DSP / control helpers:
  - envelope/shaping: `expDecayCoef`, `raisedCosine`, `softClipTanh`, `softSat`
  - filters: `Biquad` (band-pass), `OutputLpBiquad`, `DcBlocker`
  - sample playback: `readPCM16LE`, `lerpFixed` (Q12 fixed-point)
  - noise: `fillWhiteNoise`, `xorshift32`
  - controls: `PotSmoother<N>`, `PickupParam` + `checkPickup`

Example:

```cpp
#include <Mod2Common.h>

uint sliceAudio, sliceTimer;

void on_pwm_wrap();  // your audio ISR

void setup() {
  mod2::initAudioPwm(sliceAudio, sliceTimer, on_pwm_wrap);
  pinMode(mod2::BUTTON_PIN, INPUT_PULLUP);
}
```

Sketches using it: `mod2-kick`, `mod2-fm_drum`, `mod2-clap`, `mod2-hihat`, `mod2-claves`, `mod2-breakbeats`, `mod2-sample`, `mod2-radio`, `mod2-vco`, `mod2-square_vco`, `mod2-spiral`, `mod2-flux`. (`mod2-braids`/`mod2-tides` use a different PWMAudio DAC path; `mod2-mod303`/`mod2-test` use other audio backends and are not on Mod2Common.)

### Hagiwo30Common helpers

`firmwares/shared/Hagiwo30Common` provides shared constants/utilities used by the Hagiwo30 sequencer sketches:

- Board constants:
  - OLED address and dimensions
  - encoder/button/clock pins
  - 6 output channel pins
  - encoder detent count constants
- Input utility:
  - `hagiwo30::DebouncedActiveLowButton` for active-low pushbutton debouncing

Example:

```cpp
#include <Hagiwo30Common.h>

Encoder enc(hagiwo30::kEncoderPinA, hagiwo30::kEncoderPinB);
hagiwo30::DebouncedActiveLowButton button(300, HIGH);
```

The Hagiwo30 shared library now also includes a mode abstraction for building a single switchable firmware:

- `hagiwo30::SequencerMode` in `Hagiwo30SequencerMode.h`
- `hagiwo30::SequencerModeManager` in `Hagiwo30SequencerModeManager.h/.cpp`
- `firmwares/shared/Hagiwo30Sequencers` as the shared implementation for:
  - `SixChannelSequencer` (`Hagiwo30SixChannelSequencer.h/.cpp`)
  - `EuclideanSequencer` (`Hagiwo30EuclideanSequencer.h/.cpp`)
  - shared pattern banks (`Hagiwo30ProgramBanks.h`, `Hagiwo30PatternBanks.h`)

Current `SixChannelSequencer` and `EuclideanSequencer` classes implement `hagiwo30::SequencerMode`.
This enables a future sketch to instantiate both sequencers once, route calls through the manager, and switch active mode by changing `SequencerModeKind`.

Unified firmware is now available at `firmwares/hagiwo30-strides`:

- Entry sketch: `firmwares/hagiwo30-strides/hagiwo30-strides.ino`
- Runtime switch: hold the encoder button for 1.5 seconds to toggle between SixChannel and Euclidean modes

