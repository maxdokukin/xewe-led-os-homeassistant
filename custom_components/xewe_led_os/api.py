"""Lightweight HTTP client for a XeWe LED device.

Only a reachability probe for now; the real control API is added later.
"""

from __future__ import annotations

import asyncio

import aiohttp

from .const import CONNECT_TIMEOUT, DEFAULT_PORT, HEALTH_PATH


class XeweLedConnectionError(Exception):
    """Raised when the device cannot be reached."""


class XeweLedClient:
    """Talks to a XeWe LED device over HTTP."""

    def __init__(
        self,
        host: str,
        session: aiohttp.ClientSession,
        port: int = DEFAULT_PORT,
    ) -> None:
        """Initialize the client."""
        self._host = host
        self._port = port
        self._session = session

    @property
    def host(self) -> str:
        """Return the device host."""
        return self._host

    async def async_ping(self) -> bool:
        """Return True if the device answers an HTTP request.

        Reachability only: any HTTP response (even an error status) means the
        device is alive. Raises XeweLedConnectionError if it cannot be reached.
        """
        url = f"http://{self._host}:{self._port}{HEALTH_PATH}"
        try:
            async with self._session.get(
                url, timeout=aiohttp.ClientTimeout(total=CONNECT_TIMEOUT)
            ):
                return True
        except (aiohttp.ClientError, asyncio.TimeoutError) as err:
            raise XeweLedConnectionError(
                f"Could not reach XeWe LED at {self._host}: {err}"
            ) from err
