CONFIG_FILE := arduino-cli.yaml
MOD1_FQBN ?= arduino:avr:nano
MOD2_FQBN ?= rp2040:rp2040:seeed_xiao_rp2350
SHARED_LIB_DIR := firmwares/shared
RACK_PLUGIN_DIR := rack-plugins
PORT ?=
FW ?=
# ISP programmer id for the bare-ATmega328P flow (see isp-help). Any id from
# `arduino-cli board details --fqbn arduino:avr:nano --list-programmers` works:
# usbtinyisp, atmel_ice, arduinoasisp (that one also needs PORT=), ...
ISP ?= usbasp

fqbn_for = $(if $(filter mod2%,$1),$(MOD2_FQBN),$(if $(filter mod1% hagiwo30%,$1),$(MOD1_FQBN),$(MOD1_FQBN)))

# Firmwares are grouped into platform subfolders (hagiwo-mod1/, hagiwo-mod2/,
# hagiwo-30/), but a few (rabid-audio-*) still sit directly under firmwares/.
# So discover same-named `<name>/<name>.ino` sketches at either depth. The
# folder basename is still the flat build-target name; fwdir maps it back to the
# real (possibly nested) path for arduino-cli.
FIRMWARE_DIRS := $(sort $(patsubst %/,%,$(dir $(wildcard firmwares/*/*.ino firmwares/*/*/*.ino))))
FIRMWARES := $(filter-out shared,$(notdir $(FIRMWARE_DIRS)))
fwdir = $(filter %/$1,$(FIRMWARE_DIRS))

.PHONY: all everything dist clean clean-all list board-list upload upload-help \
        isp-help rack rack-dist rack-install rack-clean $(FIRMWARES)

all: dist

# Build everything this repo produces: all firmwares + the VCV Rack plugin.
everything: dist rack

# Build every firmware, skipping (not aborting on) any that fail to compile.
# Each firmware is built via its own strict rule, so `make <firmware>` still
# fails hard; only the batch build tolerates per-firmware failures.
dist:
	@failed=""; built=""; \
	for fw in $(FIRMWARES); do \
		echo "=== Building $$fw ==="; \
		if $(MAKE) --no-print-directory $$fw; then \
			built="$$built $$fw"; \
		else \
			echo ">> $$fw FAILED -- skipping"; \
			failed="$$failed $$fw"; \
		fi; \
	done; \
	echo ""; \
	echo "Built:$$built"; \
	if [ -n "$$failed" ]; then \
		echo "Skipped (failed to compile):$$failed"; \
	fi

list:
	@printf '%s\n' $(FIRMWARES)

board-list:
	arduino-cli board list --config-file $(CONFIG_FILE)

upload-help:
	@echo "Usage:"
	@echo "  make upload FW=<firmware> PORT=<port>"
	@echo "  make upload-<firmware> PORT=<port>"
	@echo ""
	@echo "Examples:"
	@echo "  make upload FW=mod1-euclidean PORT=/dev/ttyACM0"
	@echo "  make upload-mod1-euclidean PORT=/dev/ttyACM0"
	@echo ""
	@echo "This flashes over a serial bootloader. Bare ATmega328P boards"
	@echo "(rabid-audio-*) have none — see 'make isp-help' for those."
	@echo ""
	@echo "Available firmware targets:"
	@printf '  %s\n' $(FIRMWARES)

upload:
	@if [ -z "$(FW)" ]; then \
		echo "Error: FW is required."; \
		echo "Run 'make upload-help' for usage."; \
		exit 1; \
	fi
	@if [ -z "$(PORT)" ]; then \
		echo "Error: PORT is required."; \
		echo "Run 'make board-list' to discover ports."; \
		exit 1; \
	fi
	@if [ -z "$(filter $(FW),$(FIRMWARES))" ]; then \
		echo "Error: unknown firmware '$(FW)'"; \
		echo "Run 'make list' to see valid names."; \
		exit 1; \
	fi
	@$(MAKE) upload-$(FW) PORT="$(PORT)"

$(FIRMWARES):
	@mkdir -p dist/$@
	arduino-cli compile \
		--config-file $(CONFIG_FILE) \
		--fqbn $(call fqbn_for,$@) \
		--libraries $(SHARED_LIB_DIR) \
		--output-dir dist/$@ \
		$(call fwdir,$@)

upload-%: %
	@if [ -z "$(PORT)" ]; then \
		echo "Error: PORT is required for upload-$*."; \
		echo "Example: make upload-$* PORT=/dev/ttyACM0"; \
		exit 1; \
	fi
	arduino-cli upload \
		--config-file $(CONFIG_FILE) \
		--fqbn $(call fqbn_for,$*) \
		-p "$(PORT)" \
		--input-dir dist/$* \
		$(call fwdir,$*)

# ---- Bare-ATmega328P flashing over ISP ----------------------------------
# The upload rules above drive a serial bootloader, which assumes a Nano-style
# board with a USB-serial bridge. The rabid-audio-* ports run on a BARE
# ATmega328P: no bootloader to talk to, and on the CLK board no serial either —
# its 7-segment lines occupy the whole of PORTD, D0/D1 (RX/TX) included. So
# those chips are programmed in-circuit through the ICSP header instead.
#
# Two steps, and the first is easy to forget:
#
#   make fuses-rabid-audio-clk     # ONCE per chip — sets the clock fuses
#   make isp-rabid-audio-clk       # build + flash, repeat as often as you like
#
# Why fuses first: a factory ATmega328P runs its internal 8 MHz RC divided by 8,
# so 1 MHz. This firmware hard-assumes 16 MHz — sc::kClkTimerHz (15625) is
# 16 MHz / 1024 — and at factory fuses every tempo comes out 16x slow, with no
# other symptom to point at the cause. `burn-bootloader` on the Nano FQBN writes
# low_fuses=0xFF (full-swing external crystal, no CKDIV8), which is exactly what
# the CLK's 16 MHz crystal needs. It also writes Optiboot into the boot section;
# that is dead weight here, not a problem.

isp-help:
	@echo "Flashing a bare ATmega328P (rabid-audio-*) through an ISP programmer:"
	@echo "  make fuses-<firmware>          # once per chip: 16 MHz crystal fuses"
	@echo "  make isp-<firmware>            # build, then flash over ISP"
	@echo ""
	@echo "Programmer defaults to ISP=$(ISP); override for other hardware:"
	@echo "  make isp-rabid-audio-clk ISP=usbtinyisp"
	@echo "  make isp-rabid-audio-clk ISP=arduinoasisp PORT=/dev/cu.usbmodemXXXX"
	@echo ""
	@echo "List valid programmer ids:"
	@echo "  arduino-cli board details --fqbn $(MOD1_FQBN) --list-programmers"
	@echo ""
	@echo "If the programmer cannot see the chip, its SCK is likely too fast for a"
	@echo "virgin 1 MHz part (ISP clock must stay under a quarter of the target's)."
	@echo "On a USBasp, close the slow-SCK jumper (JP3) and retry fuses- first."

# Not dependent on the sketch: fuses are a property of the chip, not the build.
fuses-%:
	@case "$*" in mod2-*) \
		echo "Error: $* targets the RP2350, which has no ISP/fuse flow."; \
		exit 1;; esac
	arduino-cli burn-bootloader \
		--config-file $(CONFIG_FILE) \
		--fqbn $(call fqbn_for,$*) \
		-P "$(ISP)" \
		$(if $(PORT),-p "$(PORT)")

isp-%: %
	@case "$*" in mod2-*) \
		echo "Error: $* targets the RP2350, which has no ISP flow."; \
		echo "Flash it over USB instead: make upload-$* PORT=<port>"; \
		exit 1;; esac
	arduino-cli upload \
		--config-file $(CONFIG_FILE) \
		--fqbn $(call fqbn_for,$*) \
		-P "$(ISP)" \
		$(if $(PORT),-p "$(PORT)") \
		--input-dir dist/$* \
		$(call fwdir,$*)

clean:
	rm -rf dist

# ---- VCV Rack plugin (rack-plugins/) ------------------------------------
# Delegates to the Rack plugin Makefile, which reads the root plugin.json.
# Override the SDK location with `make rack RACK_DIR=~/Rack-SDK`.

rack:
	$(MAKE) -C $(RACK_PLUGIN_DIR) $(if $(RACK_DIR),RACK_DIR=$(RACK_DIR))

rack-dist:
	$(MAKE) -C $(RACK_PLUGIN_DIR) dist $(if $(RACK_DIR),RACK_DIR=$(RACK_DIR))

rack-install:
	$(MAKE) -C $(RACK_PLUGIN_DIR) install $(if $(RACK_DIR),RACK_DIR=$(RACK_DIR))

rack-clean:
	$(MAKE) -C $(RACK_PLUGIN_DIR) clean $(if $(RACK_DIR),RACK_DIR=$(RACK_DIR))

# Remove all build output (firmware + Rack plugin).
clean-all: clean rack-clean