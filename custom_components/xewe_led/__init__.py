"""The XEWE LED-OS integration.

Entities are created by the firmware's own MQTT discovery messages. This
integration's job is to (a) guarantee the MQTT integration is available and
(b) perform the zeroconf-discovered credential handoff (see config_flow.py).
"""

from __future__ import annotations

from homeassistant.components import mqtt
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import ConfigEntryNotReady
from homeassistant.helpers import issue_registry as ir

from .const import DOMAIN, ISSUE_MQTT_NOT_CONFIGURED, MQTT_DOCS_URL


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Set up XEWE LED-OS from a config entry."""
    if not await mqtt.async_wait_for_mqtt_client(hass):
        ir.async_create_issue(
            hass,
            DOMAIN,
            ISSUE_MQTT_NOT_CONFIGURED,
            is_fixable=False,
            is_persistent=True,
            severity=ir.IssueSeverity.ERROR,
            translation_key=ISSUE_MQTT_NOT_CONFIGURED,
            learn_more_url=MQTT_DOCS_URL,
        )
        raise ConfigEntryNotReady("MQTT integration is not available")

    # MQTT is up; clear any stale repair issue from a previous failed setup.
    ir.async_delete_issue(hass, DOMAIN, ISSUE_MQTT_NOT_CONFIGURED)
    return True


async def async_unload_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Unload a config entry."""
    return True
