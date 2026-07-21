# If RACK_DIR is not defined when calling the Makefile, default to the SDK
# vendored next to this Makefile. Override on the command line to build against
# a different Rack SDK, e.g. `make RACK_DIR=~/Rack-SDK`.
RACK_DIR ?= ./.Rack-SDK

# The canonical plugin manifest lives at the repo root (../plugin.json) so it
# sits alongside the firmware build config. Rack's plugin.mk both reads
# ./plugin.json at parse time AND packages it via rsync/cp at `make dist` --
# and neither dereferences a symlink (rsync skips it, cp embeds a broken one),
# so a symlink would yield a broken .vcvplugin. Sync a real copy in instead.
_SYNC_MANIFEST := $(shell cp -f ../plugin.json plugin.json 2>/dev/null && echo synced)

# FLAGS will be passed to both the C and C++ compiler
FLAGS +=
CFLAGS +=
CXXFLAGS +=

# Share the platform-agnostic voice cores with the firmwares. These headers
# (firmwares/shared/SynthCore/src) hold the DSP that both the Arduino sketches
# and these Rack modules call, so the algorithms live in exactly one place.
FLAGS += -I../firmwares/shared/SynthCore/src
# Desktop-only compat shim (e.g. pgmspace.h/PROGMEM) for firmware-generated
# headers such as mod2-sample's sample.h. Not on the firmware build's path.
FLAGS += -Isrc/compat

# Careful about linking to shared libraries, since you can't assume much about
# the user's environment and library search path. Static libraries are fine.
LDFLAGS +=

# WIP: Claude-generated maddie synths originals (no upstream Hagiwo/Rob Scape
# firmware) are excluded from the build until they've been reviewed/verified.
# To re-enable one, remove its .cpp from WIP_SOURCES and uncomment it in
# plugin.cpp/plugin.hpp + move its entry back to "modules" in ../plugin.json.
WIP_SOURCES := \
	src/mod2-bitcrusher.cpp \
	src/mod2-chorus.cpp \
	src/mod2-comb.cpp \
	src/mod2-delay.cpp \
	src/mod2-distortion.cpp \
	src/mod2-dynamics.cpp \
	src/mod2-filter.cpp \
	src/mod2-flanger.cpp \
	src/mod2-freeze.cpp \
	src/mod2-freq-shifter.cpp \
	src/mod2-fx.cpp \
	src/mod2-glitch-delay.cpp \
	src/mod2-granular.cpp \
	src/mod2-karplus.cpp \
	src/mod2-phaser.cpp \
	src/mod2-pitch-shifter.cpp \
	src/mod2-resonator.cpp \
	src/mod2-reverb.cpp \
	src/mod2-reverse-delay.cpp \
	src/mod2-ringmod.cpp \
	src/mod2-spectral-freeze.cpp \
	src/mod2-stutter.cpp \
	src/mod2-tape-echo.cpp \
	src/mod2-tremolo.cpp \
	src/mod2-wavefolder.cpp

# Add .cpp files to the build (minus the WIP modules above)
SOURCES += $(filter-out $(WIP_SOURCES),$(wildcard src/*.cpp))

# Add files to the ZIP package when running `make dist`
# The compiled plugin and "plugin.json" are automatically added.
DISTRIBUTABLES += res
DISTRIBUTABLES += $(wildcard LICENSE*)
DISTRIBUTABLES += $(wildcard presets)

# Include the Rack plugin Makefile framework
include $(RACK_DIR)/plugin.mk
