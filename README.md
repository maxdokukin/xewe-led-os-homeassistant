# XeWe LED — Home Assistant integration

Auto-pairing Home Assistant integration for the XeWe LED dock (ESP32). The device
is discovered by Home Assistant over mDNS and receives the MQTT broker credentials
automatically — **no broker details are typed anywhere**. Once provisioned, the
device self-publishes MQTT discovery and its entities appear on their own. This
sample firmware exposes a single **on/off switch** on one GPIO pin.

```
ESP32                                   Home Assistant
  connects to Wi-Fi (creds in sketch)
  press 'y' on Serial -> discovery mode (5 min)
  mDNS advertise _xewe-led-os._tcp ---->  Discovered in Settings > Devices & Services
  POST /provision  <-------------------  user clicks Configure and submits;
                                         HA sends broker host/port/user/pass
  MQTT connect + retained discovery -->  switch.xewe_led_os_<mac> created
```

## Requirements

- Home Assistant with the **MQTT integration** set up. The easiest path is the
  **Mosquitto broker** add-on (Settings → Add-ons → Add-on Store → Mosquitto broker
  → Install → Start), then add the auto-discovered MQTT integration. HA manages the
  broker credentials for you.
- If MQTT is not configured, this integration raises a Repair issue and refuses to
  set up until you fix it.

## Install the integration (HACS custom repository)

1. HACS → three-dot menu → **Custom repositories**.
2. Add this repository's URL, category **Integration**, then **Add**.
3. Install **XeWe LED**, then **restart Home Assistant**.

## Flash the firmware

The sample sketch is in [`firmware/xewe_led_os/xewe_led_os.ino`](firmware/xewe_led_os/xewe_led_os.ino).

**Arduino libraries** (Library Manager):

- `PubSubClient` (knolleary)
- `ArduinoJson` (bblanchon)

`WiFi.h`, `ESPmDNS.h`, `WebServer.h`, and `Preferences.h` ship with the ESP32 core.

**Before flashing**, set your Wi-Fi credentials and the output pin at the top of the
sketch:

```cpp
#define WIFI_SSID "your-wifi-ssid"
#define WIFI_PASS "your-wifi-password"
#define SWITCH_PIN 26   // GPIO the relay/LED is wired to
```

Flash to any ESP32, then open the Serial Monitor at 115200 baud.

## Pair the device

1. On boot the device connects to Wi-Fi and prints a prompt. **Press `y`** in the
   Serial Monitor to enter **discovery mode** (mDNS + provisioning endpoint), open
   for **5 minutes**.
2. In Home Assistant, the dock appears under **Settings → Devices & Services →
   Discovered**. Click **Configure** and submit.
3. Home Assistant pushes the broker credentials; the device connects and the switch
   entity `switch.xewe_led_os_<mac>` appears automatically.
4. If the 5-minute window elapses first, the Serial Monitor prints a timeout —
   press `y` again to reopen discovery. Credentials are saved to flash, so after a
   successful pairing the device reconnects on its own after reboots.

> **Broker address note:** HA often stores the broker as `core-mosquitto` or
> `localhost`, which the ESP32 cannot reach. The pairing form auto-fills a
> LAN-reachable address; override it only if your broker runs elsewhere.

## What each part does

| Path | Role |
| --- | --- |
| `custom_components/xewe_led_os/` | HA integration: MQTT prerequisite check + zeroconf pairing / credential handoff |
| `firmware/xewe_led_os/xewe_led_os.ino` | ESP32 sample: Wi-Fi onboarding, pairing, MQTT switch self-discovery, GPIO control |

## Scope

This is a minimal on/off switch sample. RGB/brightness/effects, a dedicated
per-device broker login, and an on-device PIN display are intentionally left as
follow-ups.
