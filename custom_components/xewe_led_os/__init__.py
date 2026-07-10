"""The XeWe LED integration.

For now this only establishes and monitors the connection to the device and
exposes a reachability button. The real light entity is added later.
"""

from __future__ import annotations

from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant
from homeassistant.helpers.aiohttp_client import async_get_clientsession

from .api import XeweLedClient
from .const import CONF_HOST, CONF_MAC, PLATFORMS
from .coordinator import XeweLedCoordinator

type XeweLedConfigEntry = ConfigEntry[XeweLedCoordinator]


async def async_setup_entry(hass: HomeAssistant, entry: XeweLedConfigEntry) -> bool:
    """Set up XeWe LED from a config entry."""
    session = async_get_clientsession(hass)
    client = XeweLedClient(entry.data[CONF_HOST], session)
    coordinator = XeweLedCoordinator(hass, client, entry.data[CONF_MAC])
    await coordinator.async_config_entry_first_refresh()

    entry.runtime_data = coordinator
    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)
    return True


async def async_unload_entry(hass: HomeAssistant, entry: XeweLedConfigEntry) -> bool:
    """Unload a config entry."""
    return await hass.config_entries.async_unload_platforms(entry, PLATFORMS)
