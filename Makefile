PROJECT_NAME := fileward
BUILD_DIR := build
BINARY := $(BUILD_DIR)/$(PROJECT_NAME)
RELEASE_BUILD_DIR := build-release
RELEASE_BINARY := $(RELEASE_BUILD_DIR)/$(PROJECT_NAME)

.PHONY: help setup build release check run clean install-user uninstall-user restart-user status-user logs-user reload-user health-user check-systemd

help:
	@echo "Available targets:"
	@echo "  make setup          Configure Meson build directory"
	@echo "  make build          Build fileward for development"
	@echo "  make release        Build an optimized binary for installation"
	@echo "  make check          Build and run unit, CLI and smoke tests"
	@echo "  make run            Run fileward against ~/Downloads"
	@echo "  make clean          Remove build directory"
	@echo "  make install-user   Install and start user systemd service"
	@echo "  make uninstall-user Stop and remove user systemd service"
	@echo "  make restart-user   Rebuild, install, and restart user service"
	@echo "  make health-user    Query live queue, workers and observation health"
	@echo "  make reload-user    Apply config and wait for acknowledgement"
	@echo "  make check-systemd  Test a real temporary systemd user service"
	@echo "  make status-user    Show user service status"
	@echo "  make logs-user      Follow user service logs"

setup:
	meson setup $(BUILD_DIR)

build:
	@if [ ! -d "$(BUILD_DIR)" ]; then meson setup $(BUILD_DIR); fi
	meson compile -C $(BUILD_DIR)

release:
	@if [ ! -d "$(RELEASE_BUILD_DIR)" ]; then meson setup $(RELEASE_BUILD_DIR) --buildtype=release -Db_ndebug=false; fi
	meson compile -C $(RELEASE_BUILD_DIR) $(PROJECT_NAME)

check: build
	@check_output=$$(mktemp); \
	trap 'rm -f "$$check_output"' EXIT; \
	./$(BINARY) help >"$$check_output" 2>&1; \
	status=$$?; \
	if [ $$status -eq 0 ] && grep -q "Usage:" "$$check_output"; then \
		echo "check passed"; \
	else \
		echo "check failed"; \
		cat "$$check_output"; \
		exit 1; \
	fi
	meson test -C $(BUILD_DIR) --print-errorlogs
	bash tests/integration/service-smoke.sh ./$(BINARY)
	bash tests/integration/daemon-smoke.sh ./$(BINARY)
	bash tests/integration/rules-smoke.sh ./$(BINARY)
	bash tests/integration/rules-v07-smoke.sh ./$(BINARY)
	python3 tests/integration/actions-smoke.py ./$(BINARY)
	python3 tests/integration/observability-smoke.py ./$(BINARY)
	python3 tests/integration/integrations-test.py
	python3 tests/integration/control-smoke.py ./$(BINARY)
	python3 tests/integration/saved-smoke.py ./$(BINARY)
	python3 tests/integration/journal-boundary.py ./$(BINARY)
	python3 tests/integration/installer-test.py ./$(BINARY)

run: build
	mkdir -p $(HOME)/Downloads
	./$(BINARY) run $(HOME)/Downloads

clean:
	rm -rf $(BUILD_DIR)

install-user: release
	python3 scripts/manage-user.py install --binary "$(abspath $(RELEASE_BINARY))" $(if $(CONFIG),--config "$(CONFIG)")

uninstall-user:
	python3 scripts/manage-user.py uninstall

restart-user: install-user

reload-user:
	python3 scripts/manage-user.py reload

health-user:
	python3 scripts/manage-user.py health

status-user:
	systemctl --user status fileward.service --no-pager

logs-user:
	journalctl --user -u fileward.service -f

check-systemd: build
	python3 tests/integration/systemd-smoke.py "$(abspath $(BINARY))"
