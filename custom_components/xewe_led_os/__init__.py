"""The XeWe LED integration.

Entities are created by the firmware's own MQTT discovery messages. This
integration's job is to perform the zeroconf-discovered credential handoff
(see config_flow.py).
"""

from __future__ import annotations

import asyncio
import logging

import aiohttp

from homeassistant.components import mqtt
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers.aiohttp_client import async_get_clientsession

from .const import (
    CONF_HOST,
    CONF_MAC,
    DEPROVISION_PATH,
    DISCOVERY_PREFIX,
    DOMAIN,
    PROVISION_TIMEOUT,
)

_LOGGER = logging.getLogger(__name__)


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Set up XeWe LED from a config entry."""
    return True


async def async_unload_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Unload a config entry."""
    return True


async def async_remove_entry(hass: HomeAssistant, entry: ConfigEntry) -> None:
    """Clean up when the device is deleted from Home Assistant.

    The light entity is owned by the firmware's *retained* MQTT discovery
    message, not by this integration, so removing the config entry alone leaves
    a "ghost" device that reappears on the next broker reconnect and still
    drives the hardware. Clear the retained discovery/state/availability topics
    (removing the entity from HA) and tell the device to forget its broker
    credentials (so it stops driving the hardware and republishing discovery).
    """
    mac = entry.data.get(CONF_MAC)
    host = entry.data.get(CONF_HOST)

    if mac:
        device_id = f"{DOMAIN}_{mac}"
        base = f"{DOMAIN}/{device_id}"
        # An empty retained payload deletes the retained message on the broker,
        # which is what tells HA's MQTT integration to drop the entity.
        for topic in (
            f"{DISCOVERY_PREFIX}/light/{device_id}/config",
            f"{base}/state",
            f"{base}/avail",
        ):
            try:
                await mqtt.async_publish(hass, topic, "", retain=True)
            except HomeAssistantError:
                # MQTT not configured/available; the device-side cleanup below
                # is the fallback.
                _LOGGER.debug("Could not clear retained topic %s", topic)

    if host:
        session = async_get_clientsession(hass)
        url = f"http://{host}{DEPROVISION_PATH}"
        try:
            await session.post(
                url, timeout=aiohttp.ClientTimeout(total=PROVISION_TIMEOUT)
            )
        except (aiohttp.ClientError, asyncio.TimeoutError):
            _LOGGER.debug("Device at %s did not acknowledge deprovision", host)
