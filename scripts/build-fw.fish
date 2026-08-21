#!/usr/bin/env fish
#
# build-fw.fish — compile one sketch with arduino-cli, using the repo-local
# arduino-cli.yaml and the shared Arduino library root firmwares/shared/.
# The root Makefile calls this; see `make list` for the firmware targets.
#
# License: MIT, Copyright (c) 2026 Madelyn Yeary. See LICENSE.md at the repo root.

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
