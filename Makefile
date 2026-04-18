CONFIG_FILE := arduino-cli.yaml
FQBN ?= arduino:avr:nano
SHARED_LIB_DIR := firmwares/shared

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
		--fqbn $(FQBN) \
		--libraries $(SHARED_LIB_DIR) \
		--output-dir dist/$@ \
		firmwares/$@

clean:
	rm -rf dist