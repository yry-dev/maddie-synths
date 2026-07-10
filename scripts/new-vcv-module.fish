#!/usr/bin/env fish
# new-vcv-module.fish — scaffold a new VCV Rack module that shares a core with a
# firmware (see rack-plugins/PORTING.md). Creates a stub core header, a Rack module
# .cpp, a placeholder panel SVG, and registers all three in plugin.{hpp,cpp,json}.
#
# Usage:
#   scripts/new-vcv-module.fish <Slug> "<Display Name>" "<tag1,tag2>" "<description>"
# Example:
#   scripts/new-vcv-module.fish Wavefold "Wave Folder" "Effect,Waveshaper" \
#       "West-coast wavefolder. Port of the mod2-wavefold firmware."
#
# The generated DSP + panel are STUBS — fill in the algorithm from the firmware
# and replace the placeholder panel/coords from the KiCad faceplate pipeline,
# then run scripts/check-vcv.fish.

set -l repo_root (cd (dirname (status -f)); pwd)/..

if test (count $argv) -lt 4
    echo "usage: new-vcv-module.fish <Slug> \"<Display Name>\" \"<tag1,tag2>\" \"<description>\""
    exit 2
end

python3 "$repo_root/scripts/_new_vcv_module.py" "$repo_root" $argv[1] $argv[2] $argv[3] $argv[4]
