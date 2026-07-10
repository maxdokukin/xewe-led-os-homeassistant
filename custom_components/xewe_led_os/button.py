"""Button platform for XeWe LED — a reachability probe."""

from __future__ import annotations

import logging

from homeassistant.components.button import ButtonEntity
from homeassistant.core import HomeAssistant
from homeassistant.helpers import device_registry as dr
from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from . import XeweLedConfigEntry
from .api import XeweLedConnectionError
from .const import DOMAIN
from .coordinator import XeweLedCoordinator

_LOGGER = logging.getLogger(__name__)


async def async_setup_entry(
    hass: HomeAssistant,
    entry: XeweLedConfigEntry,
    async_add_entities: AddConfigEntryEntitiesCallback,
) -> None:
    """Set up the XeWe LED button from a config entry."""
    async_add_entities([XeweLedIdentifyButton(entry.runtime_data)])


class XeweLedIdentifyButton(CoordinatorEntity[XeweLedCoordinator], ButtonEntity):
    """Pings the device to confirm it is reachable."""

    _attr_has_entity_name = True
    _attr_translation_key = "identify"

    def __init__(self, coordinator: XeweLedCoordinator) -> None:
        """Initialize the button."""
        super().__init__(coordinator)
        self._attr_unique_id = f"{coordinator.mac}_identify"
        self._attr_device_info = DeviceInfo(
            identifiers={(DOMAIN, coordinator.mac)},
            connections={(dr.CONNECTION_NETWORK_MAC, coordinator.mac)},
            name="XeWe LED",
            manufacturer="XeWe",
            model="LED",
        )

    async def async_press(self) -> None:
        """Ping the device."""
        try:
            await self.coordinator.client.async_ping()
            _LOGGER.info("XeWe LED at %s responded to ping", self.coordinator.host)
        except XeweLedConnectionError as err:
            _LOGGER.warning("XeWe LED ping failed: %s", err)
        await self.coordinator.async_request_refresh()
