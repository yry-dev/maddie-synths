CONFIG_FILE := arduino-cli.yaml
MOD1_FQBN ?= arduino:avr:nano
MOD2_FQBN ?= rp2040:rp2040:seeed_xiao_rp2350
SHARED_LIB_DIR := firmwares/shared
PORT ?=
FW ?=

fqbn_for = $(if $(filter mod2%,$1),$(MOD2_FQBN),$(if $(filter mod1% hagiwo30%,$1),$(MOD1_FQBN),$(MOD1_FQBN)))

FIRMWARE_DIRS := $(sort $(patsubst %/,%,$(dir $(wildcard firmwares/*/*.ino))))
FIRMWARES := $(filter-out shared,$(notdir $(FIRMWARE_DIRS)))

.PHONY: all dist clean list board-list upload upload-help $(FIRMWARES)

all: dist

dist: $(FIRMWARES)

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
		firmwares/$@

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
		firmwares/$*

clean:
	rm -rf dist