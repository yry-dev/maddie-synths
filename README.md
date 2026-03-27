# eurorack

My open-source / public Eurorack synthesizer module projects.

## Repository Structure

Each module lives in its own top-level directory and follows this layout:

```
<module-name>/
├── README.md        # Module description, build instructions, BOM
├── LICENSE          # Per-module license (see note below)
├── src/             # Arduino firmware source code
│   └── <module-name>/
│       └── <module-name>.ino
└── hardware/        # KiCad schematic & PCB project files
    └── <module-name>.kicad_pro
```

## Licensing

> **Each module is licensed separately.**
>
> Many of the modules in this repository are derivatives of other open-source
> projects and therefore carry their own license terms.  Please check the
> `LICENSE` file inside each module's directory before using, modifying, or
> distributing any of the files.

## Building Firmware

Firmware is built with the [Arduino CLI](https://arduino.github.io/arduino-cli/).

### Prerequisites

1. Install Arduino CLI: <https://arduino.github.io/arduino-cli/installation/>
2. Install board cores and libraries declared in `arduino-cli.yaml`:

```bash
arduino-cli core update-index --config-file arduino-cli.yaml
arduino-cli core install arduino:avr --config-file arduino-cli.yaml
```

### Compile a module

```bash
arduino-cli compile \
  --config-file arduino-cli.yaml \
  --fqbn arduino:avr:uno \
  <module-name>/src/<module-name>
```

### Upload a module

```bash
arduino-cli upload \
  --config-file arduino-cli.yaml \
  --fqbn arduino:avr:uno \
  --port /dev/ttyUSB0 \
  <module-name>/src/<module-name>
```

VS Code tasks are provided in `.vscode/tasks.json` for one-click compile and
upload from within the editor.

## Contributing

Contributions, bug reports, and suggestions are welcome.  Please open an issue
or pull request on GitHub.
