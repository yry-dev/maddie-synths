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

# Careful about linking to shared libraries, since you can't assume much about
# the user's environment and library search path. Static libraries are fine.
LDFLAGS +=

# Add .cpp files to the build
SOURCES += $(wildcard src/*.cpp)

# Add files to the ZIP package when running `make dist`
# The compiled plugin and "plugin.json" are automatically added.
DISTRIBUTABLES += res
DISTRIBUTABLES += $(wildcard LICENSE*)
DISTRIBUTABLES += $(wildcard presets)

# Include the Rack plugin Makefile framework
include $(RACK_DIR)/plugin.mk
