# AGENTS.md

Guidance for AI coding agents working in this repository.

## Project overview

This repo ships two things that work together to expose an ESP32-based **on/off
switch** in Home Assistant:

- A Home Assistant **custom integration** (`custom_components/xewe_led_os/`).
- A sample **ESP32 firmware** sketch (`firmware/xewe_led_os/xewe_led_os.ino`).

The entity is created by the **firmware's own MQTT discovery message**, not by the
integration. The firmware self-publishes a retained `homeassistant/switch/.../config`
payload and HA auto-creates the switch. The integration's *only* runtime job is:
discover the device over zeroconf/mDNS, then push HA's managed MQTT broker
credentials to the device's local `POST /provision` HTTP endpoint. Distribution is
via **HACS (custom repository)** or manual copy — this is **not** an HA-core
integration.

## Repo layout

| Path | What it is |
| --- | --- |
| `custom_components/xewe_led_os/` | The HA integration (config flow + credential handoff). |
| `custom_components/xewe_led_os/brand/` | Local brand icons (`icon.png` 256×256, `icon@2x.png` 512×512). |
| `firmware/xewe_led_os/xewe_led_os.ino` | Single-file Arduino sketch for the ESP32. |
| `hacs.json` | HACS metadata. |
| `README.md` | End-user install + pairing walkthrough. |

## Naming rule (important)

The user-facing **display name is "XeWe LED"**. Everything internal stays
`xewe_led_os` and must not be renamed to match the display name:

- domain + directory: `xewe_led_os`
- mDNS service: `_xewe-led-os._tcp`
- MQTT topics: `xewe_led_os/...`
- device id prefix: `xewe_led_os_<mac>`

## Architecture invariants

- **Entities come from MQTT discovery**, not a HA platform file. There is no
  `switch.py` and there should not be one — the firmware owns the entity.
- The integration only performs the **zeroconf discovery + `/provision` credential
  handoff**.
- **MQTT is a README-documented prerequisite, not enforced in code.** The
  in-code MQTT availability checks were intentionally removed. Do **not** re-add
  MQTT gating to `__init__.py` or `config_flow.py`.

## Firmware gotchas

- `MQTT_BUFFER_SIZE` must exceed the discovery JSON size, or PubSubClient silently
  drops the discovery publish and no entity appears.
- **Broker-host substitution matters:** HA frequently stores its broker as
  `core-mosquitto`/`localhost`, which the ESP32 cannot reach. The config flow
  substitutes a LAN-reachable IP and offers an override field — keep that behavior.
- Libraries: **PubSubClient** (knolleary) + **ArduinoJson** (bblanchon). `WiFi.h`,
  `ESPmDNS.h`, `WebServer.h`, `Preferences.h` come with the ESP32 core.
- Serial is **115200 baud**. Pressing `y` starts a **5-minute discovery window**;
  credentials are persisted in NVS (namespace `xewe`).

## Validation

- Compile-check the integration: `python -m py_compile custom_components/xewe_led_os/*.py`
- Confirm all JSON files parse (`manifest.json`, `hacs.json`, `strings.json`,
  `translations/en.json`).
- Keep `strings.json` and `translations/en.json` **in sync** — they should stay
  identical in structure.

## Docs / UI conventions

- The target HA is **2026.2+**, where the old "Add-ons" menu is now **"Apps"**
  (Settings → Apps). Use "Apps" in any user-facing documentation, not "Add-ons".
- Local `brand/` icons require **HA 2026.3+** (served via the brands proxy API).

## Distribution

- Installed via **HACS custom repository** (paste repo URL, type: Integration) or
  by manually copying `custom_components/xewe_led_os/` into `<config>/custom_components/`.
- Brand icons live in `custom_components/xewe_led_os/brand/` and take priority over
  the brands CDN.
