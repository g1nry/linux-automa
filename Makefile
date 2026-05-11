PROJECT_NAME := fileward
BUILD_DIR := build
BINARY := $(BUILD_DIR)/$(PROJECT_NAME)

USER_BIN_DIR := $(HOME)/.local/bin
USER_SYSTEMD_DIR := $(HOME)/.config/systemd/user
USER_SERVICE := $(USER_SYSTEMD_DIR)/$(PROJECT_NAME).service
SERVICE_SOURCE := systemd/user/$(PROJECT_NAME).service

.PHONY: help setup build run clean install-user uninstall-user restart-user status-user logs-user

help:
	@echo "Available targets:"
	@echo "  make setup          Configure Meson build directory"
	@echo "  make build          Build fileward"
	@echo "  make run            Run fileward against ~/Downloads"
	@echo "  make clean          Remove build directory"
	@echo "  make install-user   Install and start user systemd service"
	@echo "  make uninstall-user Stop and remove user systemd service"
	@echo "  make restart-user   Rebuild, install, and restart user service"
	@echo "  make status-user    Show user service status"
	@echo "  make logs-user      Follow user service logs"

setup:
	meson setup $(BUILD_DIR)

build:
	@if [ ! -d "$(BUILD_DIR)" ]; then meson setup $(BUILD_DIR); fi
	meson compile -C $(BUILD_DIR)

run: build
	mkdir -p $(HOME)/Downloads
	./$(BINARY) $(HOME)/Downloads

clean:
	rm -rf $(BUILD_DIR)

install-user: build
	mkdir -p $(USER_BIN_DIR)
	mkdir -p $(USER_SYSTEMD_DIR)
	mkdir -p $(HOME)/Downloads
	install -m 755 $(BINARY) $(USER_BIN_DIR)/$(PROJECT_NAME)
	install -m 644 $(SERVICE_SOURCE) $(USER_SERVICE)
	systemctl --user daemon-reload
	systemctl --user enable --now $(PROJECT_NAME).service

uninstall-user:
	-systemctl --user stop $(PROJECT_NAME).service
	-systemctl --user disable $(PROJECT_NAME).service
	rm -f $(USER_SERVICE)
	rm -f $(USER_BIN_DIR)/$(PROJECT_NAME)
	systemctl --user daemon-reload
	-systemctl --user reset-failed $(PROJECT_NAME).service

restart-user: build
	mkdir -p $(USER_BIN_DIR)
	install -m 755 $(BINARY) $(USER_BIN_DIR)/$(PROJECT_NAME)
	systemctl --user restart $(PROJECT_NAME).service

status-user:
	systemctl --user status $(PROJECT_NAME).service --no-pager

logs-user:
	journalctl --user -u $(PROJECT_NAME).service -f
