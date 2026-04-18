#!/usr/bin/env fish
set -l repo_root (cd (dirname (status -f)); pwd)/..
set -l config_file "$repo_root/arduino-cli.yaml"
set -l shared_lib_dir "$repo_root/firmwares/shared"

if test (count $argv) -lt 2
    echo "Usage: scripts/build-fw.fish <sketch_dir> <fqbn>"
    echo "Example: scripts/build-fw.fish firmwares/mod1 arduino:avr:nano"
    exit 1
end

set -l sketch_dir $argv[1]
set -l fqbn $argv[2]

arduino-cli compile \
    --config-file "$config_file" \
    --fqbn "$fqbn" \
    --libraries "$shared_lib_dir" \
    "$repo_root/$sketch_dir"
