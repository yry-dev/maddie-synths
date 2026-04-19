CONFIG_FILE := arduino-cli.yaml
MOD1_FQBN ?= arduino:avr:nano
MOD2_FQBN ?= rp2040:rp2040:seeed_xiao_rp2350
SHARED_LIB_DIR := firmwares/shared

fqbn_for = $(if $(filter mod2%,$1),$(MOD2_FQBN),$(if $(filter mod1% testbild%,$1),$(MOD1_FQBN),$(MOD1_FQBN)))

FIRMWARE_DIRS := $(sort $(patsubst %/,%,$(dir $(wildcard firmwares/*/*.ino))))
FIRMWARES := $(filter-out shared,$(notdir $(FIRMWARE_DIRS)))

.PHONY: all dist clean list $(FIRMWARES)

all: dist

dist: $(FIRMWARES)

list:
	@printf '%s\n' $(FIRMWARES)

$(FIRMWARES):
	@mkdir -p dist/$@
	arduino-cli compile \
		--config-file $(CONFIG_FILE) \
		--fqbn $(call fqbn_for,$@) \
		--libraries $(SHARED_LIB_DIR) \
		--output-dir dist/$@ \
		firmwares/$@

clean:
	rm -rf dist