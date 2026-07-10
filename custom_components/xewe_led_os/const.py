"""Constants for the XeWe LED integration."""

from __future__ import annotations

from datetime import timedelta

from homeassistant.const import Platform

DOMAIN = "xewe_led_os"

# Config entry / discovery data keys
CONF_HOST = "host"
CONF_MAC = "mac"

# Device HTTP connection
DEFAULT_PORT = 80
HEALTH_PATH = "/"  # placeholder reachability probe until the real API is defined
CONNECT_TIMEOUT = 5  # seconds

# How often the coordinator checks that the device is reachable.
SCAN_INTERVAL = timedelta(seconds=30)

PLATFORMS = [Platform.BUTTON]
