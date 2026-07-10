"""Config flow for XeWe LED.

The device advertises `_xewe-led-os._tcp.local` over mDNS; HA discovers it, the
user confirms, and we store the host + MAC after a reachability check.
"""

from __future__ import annotations

from typing import Any

from homeassistant.config_entries import ConfigFlow, ConfigFlowResult
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.service_info.zeroconf import ZeroconfServiceInfo

from .api import XeweLedClient, XeweLedConnectionError
from .const import CONF_HOST, CONF_MAC, DOMAIN


class XeweLedConfigFlow(ConfigFlow, domain=DOMAIN):
    """Handle a config flow for XeWe LED."""

    VERSION = 1

    def __init__(self) -> None:
        """Initialize the flow."""
        self._host: str | None = None
        self._mac: str | None = None

    async def async_step_zeroconf(
        self, discovery_info: ZeroconfServiceInfo
    ) -> ConfigFlowResult:
        """Handle a device discovered over mDNS."""
        self._host = str(discovery_info.ip_address)
        self._mac = discovery_info.properties.get("mac")
        if not self._mac:
            return self.async_abort(reason="no_mac")

        await self.async_set_unique_id(self._mac)
        self._abort_if_unique_id_configured(updates={CONF_HOST: self._host})

        self.context["title_placeholders"] = {"name": f"XeWe LED {self._mac[-4:]}"}
        return await self.async_step_discovery_confirm()

    async def async_step_discovery_confirm(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """Confirm the discovered device and verify it is reachable."""
        assert self._host is not None
        assert self._mac is not None

        errors: dict[str, str] = {}

        if user_input is not None:
            session = async_get_clientsession(self.hass)
            client = XeweLedClient(self._host, session)
            try:
                await client.async_ping()
            except XeweLedConnectionError:
                errors["base"] = "cannot_connect"
            else:
                return self.async_create_entry(
                    title=f"XeWe LED {self._mac[-4:]}",
                    data={CONF_HOST: self._host, CONF_MAC: self._mac},
                )

        return self.async_show_form(
            step_id="discovery_confirm",
            errors=errors,
            description_placeholders={"host": self._host},
        )
