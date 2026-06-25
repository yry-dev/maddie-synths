#!/usr/bin/env fish
# check-vcv.fish — verify harness for the vcvrack/ plugin and its shared cores.
#
# Runs the structural checks that are easy to get wrong when porting firmwares to
# VCV Rack modules (see vcvrack/PORTING.md):
#   1. the plugin builds
#   2. every res/*.svg is well-formed XML
#   3. no shared core (*Core.h) leaks a platform include (Arduino/rack/pico/Mod*Common)
#   4. each *Core.h used by a module is included by BOTH a firmware .ino and a VCV .cpp
#   5. every plugin.json module slug has a matching res/<Slug>.svg and a model registration
#
# Usage: scripts/check-vcv.fish
# Exit status is non-zero if any check fails.

set -l repo_root (cd (dirname (status -f)); pwd)/..
set -l vcv "$repo_root/vcvrack"
set -l core_dir "$repo_root/firmwares/shared/SynthCore/src"
set -l fw_dir "$repo_root/firmwares"
set -l fails 0

function _ok;   set_color green; echo "ok   $argv"; set_color normal; end
function _bad;  set_color red;   echo "FAIL $argv"; set_color normal; end

echo "== 1. build plugin =="
if make -C "$vcv" >/tmp/check-vcv-build.log 2>&1
    _ok "vcvrack plugin builds"
else
    _bad "vcvrack plugin build — see /tmp/check-vcv-build.log"
    tail -15 /tmp/check-vcv-build.log
    set fails (math $fails + 1)
end

echo "== 2. SVG well-formed =="
for svg in "$vcv"/res/*.svg
    if xmllint --noout "$svg" 2>/dev/null
        _ok (basename "$svg")
    else
        _bad (basename "$svg")" — malformed XML (would crash module load)"
        set fails (math $fails + 1)
    end
end

echo "== 3. core purity (no platform includes) =="
for hdr in "$core_dir"/*Core.h "$core_dir"/sc_*.h
    test -f "$hdr"; or continue
    if grep -nE '#include *[<"](Arduino|rack|hardware/|Mod1Common|Mod2Common)' "$hdr" >/dev/null 2>&1
        _bad (basename "$hdr")" leaks a platform include:"
        grep -nE '#include *[<"](Arduino|rack|hardware/|Mod1Common|Mod2Common)' "$hdr"
        set fails (math $fails + 1)
    else
        _ok (basename "$hdr")" pure"
    end
end

echo "== 4. each *Core.h shared by firmware AND vcv =="
for hdr in "$core_dir"/*Core.h
    set -l base (basename "$hdr" .h)            # e.g. ClavesVoice / LorenzVoice / EgCore
    # cores are included by name without extension; match in firmware .ino and vcv .cpp
    set -l in_fw  (grep -rlE "include[ ]*[<\"]$base" "$fw_dir"/*/*.ino 2>/dev/null | wc -l | string trim)
    set -l in_vcv (grep -rlE "include[ ]*[<\"]$base" "$vcv"/src/*.cpp 2>/dev/null | wc -l | string trim)
    if test "$in_fw" -ge 1 -a "$in_vcv" -ge 1
        _ok "$base shared (fw=$in_fw vcv=$in_vcv)"
    else
        _bad "$base NOT shared both ways (fw=$in_fw vcv=$in_vcv) — duplication or orphan"
        set fails (math $fails + 1)
    end
end

echo "== 5. plugin.json slugs have SVG + registration =="
python3 "$repo_root/scripts/_check_vcv_reg.py" "$vcv"; or set fails (math $fails + 1)

echo ""
if test $fails -eq 0
    set_color green; echo "ALL VCV CHECKS PASS"; set_color normal
    exit 0
else
    set_color red; echo "$fails CHECK GROUP(S) FAILED"; set_color normal
    exit 1
end
