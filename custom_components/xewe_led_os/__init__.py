"""The XeWe LED integration.

Entities are created by the firmware's own MQTT discovery messages. This
integration's job is to perform the zeroconf-discovered credential handoff
(see config_flow.py).
"""

from __future__ import annotations

from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Set up XeWe LED from a config entry."""
    return True


async def async_unload_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Unload a config entry."""
    return True
