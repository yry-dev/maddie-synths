#!/usr/bin/env fish
#
# upload-fw.fish — flash an already-built firmware to a board. Takes a firmware
# name (or a firmwares/<name> path) and uploads the artifacts arduino-cli left in
# dist/<name>/, so `make <name>` has to have run first.
#
# License: MIT, Copyright (c) 2026 Madelyn Yeary. See LICENSE.md at the repo root.

set -l repo_root (cd (dirname (status -f)); pwd)/..
set -l config_file "$repo_root/arduino-cli.yaml"

if test (count $argv) -lt 3
    echo "Usage: scripts/upload-fw.fish <firmware_name_or_sketch_dir> <fqbn> <port>"
    echo "Example: scripts/upload-fw.fish mod1-euclidean arduino:avr:nano /dev/cu.usbserial-XXXX"
    echo "Example: scripts/upload-fw.fish firmwares/hagiwo-mod1/mod1-euclidean arduino:avr:nano /dev/cu.usbserial-XXXX"
    exit 1
end

set -l firmware_arg $argv[1]
set -l fqbn $argv[2]
set -l port $argv[3]
set -l firmware_name $firmware_arg

if string match -q "firmwares/*" "$firmware_arg"
    set firmware_name (basename "$firmware_arg")
end

set -l dist_dir "$repo_root/dist/$firmware_name"

if not test -d "$dist_dir"
    echo "Error: build output not found at $dist_dir"
    echo "Run: make $firmware_name"
    exit 1
end

arduino-cli upload \
    --config-file "$config_file" \
    --fqbn "$fqbn" \
    -p "$port" \
    --input-dir "$dist_dir"
