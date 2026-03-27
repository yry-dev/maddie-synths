# template-module

> **Replace this README with a description of your module.**

## Overview

A brief description of what this module does, its panel size (HP), power
requirements (mA @ +12 V / -12 V / +5 V), and any notable features.

## Hardware

KiCad project files are located in the `hardware/` directory.

| File | Description |
|------|-------------|
| `hardware/template-module.kicad_pro` | KiCad project |

## Firmware

Source code is located in `src/template-module/`.

### Build

```bash
arduino-cli compile \
  --config-file ../../arduino-cli.yaml \
  --fqbn arduino:avr:uno \
  src/template-module
```

### Upload

```bash
arduino-cli upload \
  --config-file ../../arduino-cli.yaml \
  --fqbn arduino:avr:uno \
  --port /dev/ttyUSB0 \
  src/template-module
```

## Bill of Materials (BOM)

| Reference | Value | Description |
|-----------|-------|-------------|
| | | |

## License

See the [LICENSE](LICENSE) file in this directory.
