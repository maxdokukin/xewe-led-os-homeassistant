"""Config flow for XEWE LED-OS.

Primary path: the device advertises `_xewe-led._tcp.local`, HA discovers it, the
user enters the PIN shown by the device, and this flow forwards the (HA-managed)
MQTT broker credentials to the device's local `/provision` endpoint. From then on
the device self-publishes MQTT discovery and its entities appear automatically.
"""

from __future__ import annotations

import asyncio
from typing import Any

import aiohttp
import voluptuous as vol

from homeassistant.components import mqtt
from homeassistant.components.mqtt.const import CONF_BROKER
from homeassistant.components.network import async_get_source_ip
from homeassistant.config_entries import ConfigFlow, ConfigFlowResult
from homeassistant.const import CONF_PASSWORD, CONF_PORT, CONF_USERNAME
from homeassistant.helpers import issue_registry as ir
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.service_info.zeroconf import ZeroconfServiceInfo

from .const import (
    CONF_BROKER_OVERRIDE,
    CONF_HOST,
    CONF_MAC,
    CONF_PIN,
    DOMAIN,
    ISSUE_MQTT_NOT_CONFIGURED,
    LOCAL_BROKER_HOSTS,
    MQTT_DOCS_URL,
    PROVISION_PATH,
    PROVISION_TIMEOUT,
)


class XeweLedConfigFlow(ConfigFlow, domain=DOMAIN):
    """Handle a config flow for XEWE LED-OS."""

    VERSION = 1

    def __init__(self) -> None:
        """Initialize the flow."""
        self._host: str | None = None
        self._mac: str | None = None

    def _abort_if_mqtt_missing(self) -> ConfigFlowResult | None:
        """Abort (and raise a repair) when the MQTT integration is not set up."""
        if mqtt.mqtt_config_entry_enabled(self.hass):
            return None
        ir.async_create_issue(
            self.hass,
            DOMAIN,
            ISSUE_MQTT_NOT_CONFIGURED,
            is_fixable=False,
            is_persistent=True,
            severity=ir.IssueSeverity.ERROR,
            translation_key=ISSUE_MQTT_NOT_CONFIGURED,
            learn_more_url=MQTT_DOCS_URL,
        )
        return self.async_abort(reason=ISSUE_MQTT_NOT_CONFIGURED)

    async def async_step_zeroconf(
        self, discovery_info: ZeroconfServiceInfo
    ) -> ConfigFlowResult:
        """Handle a device discovered over mDNS."""
        if (abort := self._abort_if_mqtt_missing()) is not None:
            return abort

        self._host = str(discovery_info.ip_address)
        self._mac = discovery_info.properties.get("mac")
        if not self._mac:
            return self.async_abort(reason="no_mac")

        await self.async_set_unique_id(self._mac)
        self._abort_if_unique_id_configured(updates={CONF_HOST: self._host})

        self.context["title_placeholders"] = {"name": f"XEWE Dock {self._mac[-4:]}"}
        return await self.async_step_pair()

    async def async_step_user(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """Manual entry fallback: ask for the device IP, then pair."""
        if (abort := self._abort_if_mqtt_missing()) is not None:
            return abort

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
        """Ask for the PIN, then push broker credentials to the device."""
        assert self._host is not None

        broker_default = await self._suggested_broker_host()
        errors: dict[str, str] = {}

        if user_input is not None:
            broker = user_input.get(CONF_BROKER_OVERRIDE) or broker_default
            error = await self._async_provision(
                pin=user_input[CONF_PIN], broker_host=broker
            )
            if error is None:
                return self.async_create_entry(
                    title=f"XEWE Dock {(self._mac or self._host)[-4:]}",
                    data={CONF_HOST: self._host, CONF_MAC: self._mac},
                )
            errors["base"] = error

        schema = self.add_suggested_values_to_schema(
            vol.Schema(
                {
                    vol.Required(CONF_PIN): str,
                    vol.Optional(CONF_BROKER_OVERRIDE): str,
                }
            ),
            {CONF_BROKER_OVERRIDE: broker_default},
        )
        return self.async_show_form(
            step_id="pair",
            data_schema=schema,
            errors=errors,
            description_placeholders={"host": self._host},
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

    async def _async_provision(self, pin: str, broker_host: str | None) -> str | None:
        """POST broker credentials to the device. Return an error key or None."""
        if not broker_host:
            return "no_broker"

        data = self._mqtt_data()
        payload = {
            "host": broker_host,
            "port": data.get(CONF_PORT, 1883),
            "user": data.get(CONF_USERNAME) or "",
            "pass": data.get(CONF_PASSWORD) or "",
            "pin": pin,
        }
        session = async_get_clientsession(self.hass)
        url = f"http://{self._host}{PROVISION_PATH}"
        try:
            async with session.post(
                url,
                json=payload,
                timeout=aiohttp.ClientTimeout(total=PROVISION_TIMEOUT),
            ) as resp:
                if resp.status == 403:
                    return "invalid_pin"
                if resp.status != 200:
                    return "cannot_connect"
        except (aiohttp.ClientError, asyncio.TimeoutError):
            return "cannot_connect"
        return None
