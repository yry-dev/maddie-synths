#!/usr/bin/env fish

set -l repo_root (cd (dirname (status -f)); pwd)/..
set -l config_file "$repo_root/arduino-cli.yaml"

if not type -q arduino-cli
    echo "Error: arduino-cli is not installed or not on PATH." >&2
    exit 1
end

if not test -f "$config_file"
    echo "Error: config file not found: $config_file" >&2
    exit 1
end

function run_checked
    set -l label $argv[1]
    set -e argv[1]

    echo "==> $label"
    $argv
    or begin
        echo "Error: $label failed." >&2
        return 1
    end
end

run_checked "Updating board index" \
    arduino-cli core update-index --config-file "$config_file"
or exit 1

run_checked "Installing Arduino AVR core (arduino:avr)" \
    arduino-cli core install arduino:avr --config-file "$config_file"
or exit 1

run_checked "Installing RP2040 core (rp2040:rp2040)" \
    arduino-cli core install rp2040:rp2040 --config-file "$config_file"
or exit 1

run_checked "Installing prerequisite libraries" \
    arduino-cli lib install \
    Encoder \
    FastGPIO \
    "Adafruit GFX Library" \
    "Adafruit SSD1306" \
    --config-file "$config_file"
or exit 1

echo "Done. Arduino CLI cores and libraries are ready."
