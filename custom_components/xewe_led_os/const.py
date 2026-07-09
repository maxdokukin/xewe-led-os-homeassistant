"""Constants for the XeWe LED integration."""

from __future__ import annotations

DOMAIN = "xewe_led_os"

# Config entry / discovery data keys
CONF_HOST = "host"
CONF_MAC = "mac"
CONF_BROKER_OVERRIDE = "broker_override"

# Firmware HTTP provisioning endpoint
PROVISION_PATH = "/provision"
PROVISION_TIMEOUT = 10  # seconds

# Repair issue id / translation key raised when MQTT is not set up
ISSUE_MQTT_NOT_CONFIGURED = "mqtt_not_configured"
MQTT_DOCS_URL = "https://www.home-assistant.io/integrations/mqtt/"

# Broker hostnames that are only reachable from *inside* the HA host and must be
# rewritten to a LAN-reachable address before being handed to the ESP32.
LOCAL_BROKER_HOSTS = frozenset({"localhost", "127.0.0.1", "::1", "core-mosquitto"})
