# XeWe LED — Home Assistant integration

Control a XeWe LED dock (an ESP32 running the sample firmware in this repo) from
Home Assistant as a simple **on/off switch**.

The goal is a hands-off pairing experience: you press one key on the device, it
shows up in Home Assistant on its own, you click **Configure**, and the switch
appears. **You never type any broker addresses or passwords** — Home Assistant
hands the device everything it needs behind the scenes.

---

## How it works (in one picture)

```
ESP32 device                              Home Assistant
─────────────                             ──────────────
connects to your Wi-Fi
you press 'y' → "discovery mode" (5 min)
announces itself on the network   ─────►  appears under "Discovered"
                                          you click Configure and Submit
receives broker address + login   ◄─────  HA sends its MQTT details
connects and reports its switch   ─────►  switch.xewe_led_os_<id> is created
```

You do this once. After that the device remembers everything and reconnects by
itself on every reboot.

---

## Before you start: two prerequisites

### 1. MQTT (the messaging system HA and the device talk over)

Home Assistant and the device communicate through **MQTT**, a lightweight
messaging system. You need it running in Home Assistant first.

Easiest path (Home Assistant OS / Supervised):

1. **Settings → Add-ons → Add-on Store**.
2. Search for **Mosquitto broker**, then **Install** and **Start** it.
3. Go to **Settings → Devices & Services**. Home Assistant will offer to set up
   the discovered **MQTT** integration — click **Configure** and accept the
   defaults. Home Assistant now manages the MQTT login for you.

> If MQTT is not set up, this integration will refuse to load and show a Repair
> notice telling you to configure MQTT first.

### 2. HACS (the store for community integrations)

This integration is not built into Home Assistant, so you install it through
**HACS** (Home Assistant Community Store) — an add-on that lets you install
integrations that aren't shipped with Home Assistant by default.

If you don't already have HACS, follow the official install guide:
https://www.hacs.xyz/docs/use/download/download/ — then continue below.

---

## Step 1 — Install this integration via HACS

1. Open **HACS** in the Home Assistant sidebar.
2. Click the **three-dot menu** (top right) → **Custom repositories**.
3. Paste this repository's URL, set **Type / Category** to **Integration**, and
   click **Add**.
4. Find **XeWe LED** in the list, open it, and click **Download**.
5. **Restart Home Assistant** (Settings → System → Restart).

## Step 2 — Flash the firmware onto the ESP32

The sample sketch is [`firmware/xewe_led_os/xewe_led_os.ino`](firmware/xewe_led_os/xewe_led_os.ino).

1. In the Arduino IDE, install these libraries (**Tools → Manage Libraries**):
   - **PubSubClient** (by knolleary)
   - **ArduinoJson** (by bblanchon)

   (`WiFi.h`, `ESPmDNS.h`, `WebServer.h`, and `Preferences.h` already come with
   the ESP32 board package.)

2. Open the sketch and edit the settings at the top — put in your Wi-Fi details
   and the GPIO pin your LED/relay is wired to:

   ```cpp
   #define WIFI_SSID "your-wifi-ssid"
   #define WIFI_PASS "your-wifi-password"
   #define SWITCH_PIN 26   // GPIO the relay/LED is wired to
   ```

3. Flash it to the ESP32, then open the **Serial Monitor at 115200 baud**.

The device must be on the **same Wi-Fi network** as Home Assistant.

## Step 3 — Pair the device

1. In the Serial Monitor the device reports it connected to Wi-Fi and prints a
   prompt. **Press `y`** to put it into **discovery mode** for **5 minutes**.
2. In Home Assistant, go to **Settings → Devices & Services**. The dock appears
   under **Discovered**. Click **Configure**, then **Submit**.
3. That's it — Home Assistant sends the device its MQTT details, the device
   connects, and a switch entity named `switch.xewe_led_os_<id>` is created.
   Toggle it and the GPIO pin turns on/off.

**If the 5-minute window runs out first**, the Serial Monitor prints a timeout —
just press `y` again to reopen it. Once paired, the credentials are saved on the
device, so it reconnects automatically after reboots or power loss.

> **If pairing succeeds but the device can't connect to MQTT:** Home Assistant
> sometimes stores its broker address as `core-mosquitto` or `localhost`, which
> the ESP32 can't reach. The Configure form auto-fills a network-reachable
> address for you; only change the "MQTT broker address" field if your broker
> runs on a different machine.

---

## What's in this repo

| Path | What it is |
| --- | --- |
| `custom_components/xewe_led_os/` | The Home Assistant integration: checks that MQTT is ready, discovers the device, and hands it the broker credentials. |
| `firmware/xewe_led_os/xewe_led_os.ino` | The ESP32 sample firmware: connects to Wi-Fi, pairs, and exposes one GPIO as an on/off switch. |

## Scope

This is a minimal **on/off switch** sample. RGB / brightness / effects, a
dedicated per-device broker login, and an on-device pairing display are
intentionally left as follow-ups.
