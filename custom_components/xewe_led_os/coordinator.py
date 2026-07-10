"""Reachability coordinator for a XeWe LED device."""

from __future__ import annotations

import logging

from homeassistant.core import HomeAssistant
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator, UpdateFailed

from .api import XeweLedClient, XeweLedConnectionError
from .const import DOMAIN, SCAN_INTERVAL

_LOGGER = logging.getLogger(__name__)


class XeweLedCoordinator(DataUpdateCoordinator[bool]):
    """Polls the device to track whether it is reachable."""

    def __init__(
        self,
        hass: HomeAssistant,
        client: XeweLedClient,
        mac: str,
    ) -> None:
        """Initialize the coordinator."""
        super().__init__(
            hass,
            _LOGGER,
            name=DOMAIN,
            update_interval=SCAN_INTERVAL,
        )
        self.client = client
        self.mac = mac

    @property
    def host(self) -> str:
        """Return the device host."""
        return self.client.host

    async def _async_update_data(self) -> bool:
        """Check that the device is reachable."""
        try:
            return await self.client.async_ping()
        except XeweLedConnectionError as err:
            raise UpdateFailed(str(err)) from err
