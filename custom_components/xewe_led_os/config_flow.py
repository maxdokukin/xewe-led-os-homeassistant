"""Config flow for XeWe LED.

Primary path: the device (in discovery mode) advertises `_xewe-led-os._tcp.local`,
HA discovers it, the user confirms, and this flow forwards the (HA-managed) MQTT
broker credentials to the device's local `/provision` endpoint. From then on the
device self-publishes MQTT discovery and its entities appear automatically.
"""

from __future__ import annotations

import asyncio
from typing import Any

import aiohttp
import voluptuous as vol

from homeassistant.components.mqtt.const import CONF_BROKER
from homeassistant.components.network import async_get_source_ip
from homeassistant.config_entries import ConfigFlow, ConfigFlowResult
from homeassistant.const import CONF_PASSWORD, CONF_PORT, CONF_USERNAME
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.service_info.zeroconf import ZeroconfServiceInfo

from .const import (
    CONF_HOST,
    CONF_MAC,
    DOMAIN,
    LOCAL_BROKER_HOSTS,
    PROVISION_PATH,
    PROVISION_TIMEOUT,
)


class XeweLedConfigFlow(ConfigFlow, domain=DOMAIN):
    """Handle a config flow for XeWe LED."""

    VERSION = 1

    def __init__(self) -> None:
        """Initialize the flow."""
        self._host: str | None = None
        self._mac: str | None = None
        self._name: str | None = None

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

        # The device advertises its name over mDNS; use it as the discovery card
        # title (falling back to a mac-suffixed placeholder if it's absent).
        self._name = discovery_info.properties.get("name") or (
            f"XeWe LED {self._mac[-4:]}"
        )
        self.context["title_placeholders"] = {"name": self._name}
        return await self.async_step_pair()

    async def async_step_user(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """Manual entry fallback: ask for the device IP, then pair."""
        if user_input is not None:
            self._host = user_input[CONF_HOST]
            return await self.async_step_pair()

        return self.async_show_form(
            step_id="user",
            data_schema=vol.Schema({vol.Required(CONF_HOST): str}),
        )

    async def async_step_pair(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """Confirm, then auto-provision the device with HA's MQTT broker.

        No broker details are shown: Home Assistant's own MQTT credentials are
        forwarded to the device automatically. This step is only a confirmation
        (discovery flows require one interaction before creating an entry).
        """
        assert self._host is not None
        errors: dict[str, str] = {}

        if user_input is not None:
            mqtt_data = self._mqtt_data()
            error = await self._async_provision(
                broker_host=await self._suggested_broker_host(),
                username=mqtt_data.get(CONF_USERNAME, ""),
                password=mqtt_data.get(CONF_PASSWORD, ""),
            )
            if error is None:
                return self.async_create_entry(
                    title=self._name
                    or f"XeWe LED {(self._mac or self._host)[-4:]}",
                    data={CONF_HOST: self._host, CONF_MAC: self._mac},
                )
            errors["base"] = error

        return self.async_show_form(
            step_id="pair",
            errors=errors,
        )

    def _mqtt_data(self) -> dict[str, Any]:
        """Return the data dict of the MQTT config entry."""
        entries = self.hass.config_entries.async_entries("mqtt")
        return dict(entries[0].data) if entries else {}

    async def _suggested_broker_host(self) -> str | None:
        """Broker host to hand the device, LAN-reachable from the ESP32."""
        broker = self._mqtt_data().get(CONF_BROKER)
        if broker and broker not in LOCAL_BROKER_HOSTS:
            return broker
        # Broker is only reachable inside the HA host; use the LAN IP that HA
        # would use to reach the device instead.
        if self._host:
            try:
                return await async_get_source_ip(self.hass, target_ip=self._host)
            except (OSError, RuntimeError):
                return None
        return None

    async def _async_provision(
        self, broker_host: str | None, username: str, password: str
    ) -> str | None:
        """POST broker credentials to the device. Return an error key or None."""
        if not broker_host:
            return "no_broker"

        data = self._mqtt_data()
        payload = {
            "host": broker_host,
            "port": data.get(CONF_PORT, 1883),
            "user": username or "",
            "pass": password or "",
        }
        session = async_get_clientsession(self.hass)
        url = f"http://{self._host}{PROVISION_PATH}"
        try:
            async with session.post(
                url,
                json=payload,
                timeout=aiohttp.ClientTimeout(total=PROVISION_TIMEOUT),
            ) as resp:
                if resp.status != 200:
                    return "cannot_connect"
        except (aiohttp.ClientError, asyncio.TimeoutError):
            return "cannot_connect"
        return None
