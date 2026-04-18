#!/usr/bin/env fish
set -l repo_root (cd (dirname (status -f)); pwd)/..
set -l config_file "$repo_root/arduino-cli.yaml"

if test (count $argv) -lt 3
    echo "Usage: scripts/upload-fw.fish <sketch_dir> <fqbn> <port>"
    echo "Example: scripts/upload-fw.fish firmwares/mod1 arduino:avr:nano /dev/cu.usbserial-XXXX"
    exit 1
end

set -l sketch_dir $argv[1]
set -l fqbn $argv[2]
set -l port $argv[3]

arduino-cli upload \
    --config-file "$config_file" \
    --fqbn "$fqbn" \
    -p "$port" \
    "$repo_root/$sketch_dir"
