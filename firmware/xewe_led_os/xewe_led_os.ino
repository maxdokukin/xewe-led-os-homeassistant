/*
 * XeWe LED dock — sample ESP32 firmware (mock full data model)
 * -----------------------------------------------------------
 * Relays the FULL LED data model to Home Assistant over MQTT auto-discovery,
 * using MOCK values that mirror the real device (C:/dev/Codebase/xewe-led-os):
 *
 *   - a device NAME (carried by the shared MQTT `device` block),
 *   - on/off STATE, BRIGHTNESS (0-255), and the active MODE (7 of them),
 *     all on one JSON-schema MQTT `light` (mode = the light's `effect`),
 *   - per-mode CONTROLS as dynamic MQTT `number` entities — each mode exposes a
 *     DIFFERENT set of params, so on every mode change we clear the previous
 *     mode's number discovery and publish the new mode's.
 *
 * Provisioning is unchanged from the on/off sample:
 *   1. Connects to Wi-Fi using the credentials in wifi_c.h.
 *   2. If not yet provisioned, press 'y' on Serial to enter DISCOVERY MODE:
 *        - advertise mDNS service _xewe-led-os._tcp.local (TXT: mac, fw)
 *        - accept POST /provision {host,port,user,pass} (no PIN)
 *   3. On provision -> store broker creds in NVS, connect to MQTT, publish the
 *      RETAINED discovery config for the light + current mode's params.
 *
 * MQTT is used (instead of plain HTTP) so device-originated state changes push
 * to Home Assistant immediately, rather than waiting for HA to poll.
 *
 * Required Arduino libraries (Library Manager):
 *   - PubSubClient (knolleary)
 *   - ArduinoJson (bblanchon)
 * Bundled with the ESP32 core: WiFi.h, ESPmDNS.h, WebServer.h, Preferences.h
 */

#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// ---- User configuration -----------------------------------------------------
// Wi-Fi credentials live in wifi_c.h (gitignored). Copy wifi.h.example to
// wifi_c.h and fill in WIFI_SSID / WIFI_PASS.
#include "wifi_c.h"

#define LED_PIN 8               // GPIO the LED/relay is wired to
#define LED_ACTIVE_HIGH true    // set false if your LED/relay is active-low
#define FW_VERSION "0.4.0"
#define MQTT_BUFFER_SIZE 2048   // MUST exceed the discovery JSON (effect_list +
                                // device block push the light config past 1024)

// ---- Mock data model --------------------------------------------------------
// Mirrors the real device's ModeController::get_all_modes_json() descriptor.
// `type`: 'b' = basic (always shown), 'a' = additional (HA entity_category=config).
struct Param {
  const char *key;
  const char *name;
  uint16_t min;
  uint16_t max;
  uint16_t def;
  uint16_t step;
  char type;
};

struct Mode {
  const char *name;
  const Param *params;
  uint8_t count;
};

static const Param SOLID_PARAMS[] = {
    {"hue", "Hue", 0, 255, 0, 1, 'b'},
    {"sat", "Saturation", 0, 255, 255, 1, 'b'},
};

static const Param COLOR_FADE_PARAMS[] = {
    {"hue", "Hue", 0, 255, 195, 1, 'b'},
    {"sat", "Saturation", 0, 245, 245, 1, 'b'},
    {"speed", "Speed", 1, 50, 4, 1, 'a'},
    {"fire_step", "Fire Step", 1, 255, 20, 1, 'a'},
    {"h_gap", "Hue Gap", 0, 65535, 15000, 100, 'a'},
    {"min_bright", "Min Brightness", 0, 255, 150, 1, 'a'},
};

static const Param COLOR_FADE_TWO_ZONE_PARAMS[] = {
    {"hue", "Hue", 0, 255, 81, 1, 'b'},
    {"hue_b", "Hue B", 0, 255, 225, 1, 'b'},
    {"blend", "Blend", 2, 255, 150, 1, 'a'},
    {"speed", "Speed", 1, 50, 3, 1, 'a'},
    {"fire_step", "Fire Step", 1, 255, 10, 1, 'a'},
    {"min_bright", "Min Brightness", 0, 255, 245, 1, 'a'},
    {"min_sat", "Min Saturation", 0, 255, 215, 1, 'a'},
};

static const Param BRIGHTNESS_FADE_PARAMS[] = {
    {"hue", "Hue", 0, 255, 0, 1, 'b'},
    {"sat", "Saturation", 0, 255, 255, 1, 'b'},
    {"speed", "Speed", 1, 50, 5, 1, 'a'},
    {"noise_step", "Noise Step", 1, 255, 10, 1, 'a'},
    {"min_bright", "Min Brightness", 0, 255, 10, 1, 'a'},
};

static const Param PULSE_PARAMS[] = {
    {"hue", "Hue", 0, 255, 0, 1, 'b'},
    {"sat", "Saturation", 0, 255, 255, 1, 'b'},
    {"speed", "Speed", 1, 255, 30, 1, 'a'},
};

static const Param RAINBOW_PARAMS[] = {
    {"speed", "Speed", 1, 20, 5, 1, 'b'},
    {"density", "Density", 1, 30, 10, 1, 'a'},
};

static const Param CHRISTMAS_LIGHTS_PARAMS[] = {
    {"density", "Density", 1, 10, 1, 1, 'b'},
    {"speed", "Flicker", 0, 20, 5, 1, 'a'},
};

#define MODE_ENTRY(name, arr) {name, arr, sizeof(arr) / sizeof(arr[0])}
static const Mode MODES[] = {
    MODE_ENTRY("Solid", SOLID_PARAMS),
    MODE_ENTRY("Color Fade", COLOR_FADE_PARAMS),
    MODE_ENTRY("Color Fade Two Zone", COLOR_FADE_TWO_ZONE_PARAMS),
    MODE_ENTRY("Brightness Fade", BRIGHTNESS_FADE_PARAMS),
    MODE_ENTRY("Pulse", PULSE_PARAMS),
    MODE_ENTRY("Rainbow", RAINBOW_PARAMS),
    MODE_ENTRY("Christmas Lights", CHRISTMAS_LIGHTS_PARAMS),
};
static const uint8_t MODE_COUNT = sizeof(MODES) / sizeof(MODES[0]);
#define MAX_PARAMS 8  // must be >= the largest mode's param count

// ---- Globals ----------------------------------------------------------------
Preferences prefs;
WiFiClient net;
PubSubClient mqtt(net);
WebServer server(80);

String deviceId;    // "xewe_led_os_aabbccddeeff"
String macHex;      // "aabbccddeeff"
String baseTopic;   // "xewe_led_os/<deviceId>"
String availTopic, lightCmdTopic, lightStateTopic, lightDiscoveryTopic;

String mqttHost, mqttUser, mqttPass;
uint16_t mqttPort = 1883;

bool provisioned = false;
bool pairing = false;

// Mock device state.
String deviceName = "XeWe LED";
bool lightOn = false;
uint8_t brightness = 255;
uint8_t modeId = 0;
uint16_t paramValues[MAX_PARAMS];  // values for the CURRENT mode's params

// ---- Helpers ----------------------------------------------------------------
String macToHex() {
  String m = WiFi.macAddress();  // "AA:BB:CC:DD:EE:FF"
  m.replace(":", "");
  m.toLowerCase();
  return m;
}

void buildTopics() {
  deviceId = "xewe_led_os_" + macHex;
  baseTopic = "xewe_led_os/" + deviceId;
  availTopic = baseTopic + "/avail";
  lightCmdTopic = baseTopic + "/light/set";
  lightStateTopic = baseTopic + "/light/state";
  lightDiscoveryTopic = "homeassistant/light/" + deviceId + "/config";
}

void applyLight(bool on) {
  lightOn = on;
  digitalWrite(LED_PIN, (on == LED_ACTIVE_HIGH) ? HIGH : LOW);
}

int modeIdByName(const char *name) {
  for (uint8_t i = 0; i < MODE_COUNT; i++) {
    if (strcmp(MODES[i].name, name) == 0) return i;
  }
  return -1;
}

// Per-mode number entity topics (derived from the current mode's param keys).
String paramSetTopic(const char *key) {
  return baseTopic + "/param/" + key + "/set";
}
String paramStateTopic(const char *key) {
  return baseTopic + "/param/" + key + "/state";
}
String paramDiscoveryTopic(const char *key) {
  return "homeassistant/number/" + deviceId + "/" + key + "/config";
}

void printEnablePrompt() {
  Serial.println("----------------------------------------");
  Serial.println("[setup] Press 'y' to enable Home Assistant discovery");
  Serial.println("----------------------------------------");
}

// ---- Persistence ------------------------------------------------------------
bool loadCreds() {
  prefs.begin("xewe", true);
  bool has = prefs.isKey("mhost");
  if (has) {
    mqttHost = prefs.getString("mhost", "");
    mqttPort = prefs.getUShort("mport", 1883);
    mqttUser = prefs.getString("muser", "");
    mqttPass = prefs.getString("mpass", "");
  }
  prefs.end();
  return has && mqttHost.length() > 0;
}

void saveCreds(const String &host, uint16_t port, const String &user,
               const String &pass) {
  prefs.begin("xewe", false);
  prefs.putString("mhost", host);
  prefs.putUShort("mport", port);
  prefs.putString("muser", user);
  prefs.putString("mpass", pass);
  prefs.end();
  mqttHost = host;
  mqttPort = port;
  mqttUser = user;
  mqttPass = pass;
}

// ---- Discovery mode (mDNS + /provision) -------------------------------------
void handleProvision() {
  if (!pairing) {
    server.send(403, "application/json", "{\"error\":\"not_pairing\"}");
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"error\":\"bad_json\"}");
    return;
  }

  saveCreds(doc["host"] | "", doc["port"] | 1883, doc["user"] | "",
            doc["pass"] | "");
  server.send(200, "application/json", "{\"ok\":true}");
  Serial.println("[pair] provisioned; leaving discovery mode");

  provisioned = true;
  pairing = false;
  MDNS.end();  // stop advertising; keep the HTTP server up for /deprovision
}

// Forward declarations for the deprovision cleanup.
void clearParamDiscovery();

// Factory reset: clear the retained MQTT discovery so HA drops every entity
// (the light AND the current mode's number entities), wipe stored broker creds,
// and reboot back into discovery mode. This is what makes "delete the device in
// Home Assistant" actually release the hardware instead of leaving a ghost
// behind. NOTE: the HA-side async_remove_entry can't know the dynamic param
// keys, so this device-side path is the authoritative cleanup for the numbers.
void handleDeprovision() {
  server.send(200, "application/json", "{\"ok\":true}");
  Serial.println("[deprovision] clearing broker creds + retained discovery");

  if (mqtt.connected()) {
    clearParamDiscovery();  // drop the current mode's number entities
    mqtt.publish(lightDiscoveryTopic.c_str(), "", true);  // empty retained = del
    mqtt.publish(lightStateTopic.c_str(), "", true);
    mqtt.publish(availTopic.c_str(), "offline", true);
    mqtt.loop();
    delay(100);  // let the socket flush before we drop it
    mqtt.disconnect();
  }

  prefs.begin("xewe", false);
  prefs.clear();
  prefs.end();
  delay(200);
  ESP.restart();
}

// The HTTP server runs in every state so a provisioned device can still be
// factory-reset via /deprovision. /provision self-guards on `pairing`.
void startWebServer() {
  server.on("/provision", HTTP_POST, handleProvision);
  server.on("/deprovision", HTTP_POST, handleDeprovision);
  server.begin();
}

void startPairing() {
  pairing = true;

  String host = "xewe-led-os-" + macHex.substring(6);
  if (MDNS.begin(host.c_str())) {
    MDNS.addService("_xewe-led-os", "_tcp", 80);
    MDNS.addServiceTxt("_xewe-led-os", "_tcp", "mac", macHex);
    MDNS.addServiceTxt("_xewe-led-os", "_tcp", "fw", FW_VERSION);
  }

  Serial.println("========================================");
  Serial.println("[pair] DISCOVERY MODE active (stays on until provisioned)");
  Serial.printf("[pair] mDNS: %s._xewe-led-os._tcp.local  (%s)\n",
                host.c_str(), WiFi.localIP().toString().c_str());
  Serial.println("[pair] Open HA > Settings > Devices & Services and configure");
  Serial.println("       the discovered XeWe LED.");
  Serial.println("========================================");
}

// ---- MQTT: discovery --------------------------------------------------------
// Shared device block so HA groups the light + all number entities under one
// device. The device `name` is how the mock device name reaches HA.
void addDevice(JsonDocument &doc) {
  JsonObject dev = doc["device"].to<JsonObject>();
  dev["identifiers"][0] = deviceId;
  dev["name"] = deviceName;
  dev["manufacturer"] = "XeWe";
  dev["model"] = "LED";
  dev["sw_version"] = FW_VERSION;
}

// One JSON-schema light carrying state + brightness + effect(=mode).
void publishLightDiscovery() {
  JsonDocument doc;
  doc["~"] = baseTopic;
  doc["schema"] = "json";
  doc["name"] = "XeWe LED";
  doc["unique_id"] = deviceId + "_light";
  doc["command_topic"] = "~/light/set";
  doc["state_topic"] = "~/light/state";
  doc["availability_topic"] = "~/avail";
  doc["payload_available"] = "online";
  doc["payload_not_available"] = "offline";
  doc["brightness"] = true;
  doc["effect"] = true;
  JsonArray fx = doc["effect_list"].to<JsonArray>();
  for (uint8_t i = 0; i < MODE_COUNT; i++) fx.add(MODES[i].name);
  addDevice(doc);

  String payload;
  serializeJson(doc, payload);
  bool ok = mqtt.publish(lightDiscoveryTopic.c_str(), payload.c_str(), true);
  Serial.printf("[mqtt] light discovery (%u bytes): %s\n", payload.length(),
                ok ? "ok" : "FAILED (raise MQTT_BUFFER_SIZE?)");
}

// A `number` entity per param of the CURRENT mode. Additional ('a') params go
// under entity_category=config so they're tucked away in HA's advanced controls.
void publishParamDiscovery() {
  const Mode &m = MODES[modeId];
  for (uint8_t i = 0; i < m.count; i++) {
    const Param &p = m.params[i];
    JsonDocument doc;
    doc["~"] = baseTopic;
    doc["name"] = p.name;
    doc["unique_id"] = deviceId + "_" + p.key;
    doc["command_topic"] = String("~/param/") + p.key + "/set";
    doc["state_topic"] = String("~/param/") + p.key + "/state";
    doc["availability_topic"] = "~/avail";
    doc["min"] = p.min;
    doc["max"] = p.max;
    doc["step"] = p.step;
    if (p.type == 'a') doc["entity_category"] = "config";
    addDevice(doc);

    String payload;
    serializeJson(doc, payload);
    mqtt.publish(paramDiscoveryTopic(p.key).c_str(), payload.c_str(), true);
  }
  Serial.printf("[mqtt] param discovery for mode '%s' (%u params)\n", m.name,
                m.count);
}

// Empty retained payloads delete the current mode's number discovery + state so
// HA drops those entities before we switch to a different mode's controls.
void clearParamDiscovery() {
  const Mode &m = MODES[modeId];
  for (uint8_t i = 0; i < m.count; i++) {
    mqtt.publish(paramDiscoveryTopic(m.params[i].key).c_str(), "", true);
    mqtt.publish(paramStateTopic(m.params[i].key).c_str(), "", true);
  }
}

// ---- MQTT: state ------------------------------------------------------------
// Publish the light state (on/off + brightness + effect). Call this on EVERY
// state change, not just on MQTT commands — a physical button / HomeKit / Alexa
// change must also be published here, otherwise Home Assistant won't see it
// (that push is the whole reason this firmware uses MQTT instead of HTTP poll).
void publishLightState() {
  JsonDocument doc;
  doc["state"] = lightOn ? "ON" : "OFF";
  doc["brightness"] = brightness;
  doc["effect"] = MODES[modeId].name;
  String payload;
  serializeJson(doc, payload);
  mqtt.publish(lightStateTopic.c_str(), payload.c_str(), true);
}

void publishParamState(uint8_t index) {
  const Param &p = MODES[modeId].params[index];
  mqtt.publish(paramStateTopic(p.key).c_str(), String(paramValues[index]).c_str(),
               true);
}

void publishParamStates() {
  for (uint8_t i = 0; i < MODES[modeId].count; i++) publishParamState(i);
}

// Switch modes: reset param values to the new mode's defaults, swap the number
// entities (clear old, publish new), and publish fresh param states.
void setMode(uint8_t newId) {
  clearParamDiscovery();  // clears the OLD (still-current) mode's numbers
  modeId = newId;
  const Mode &m = MODES[modeId];
  for (uint8_t i = 0; i < m.count; i++) paramValues[i] = m.params[i].def;
  publishParamDiscovery();
  publishParamStates();
}

// ---- MQTT: commands ---------------------------------------------------------
void handleLightCommand(const String &json) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return;

  if (doc["state"].is<const char *>()) {
    applyLight(strcmp(doc["state"], "ON") == 0);
  }
  if (doc["brightness"].is<int>()) {
    int b = doc["brightness"];
    brightness = (uint8_t)constrain(b, 0, 255);
  }
  if (doc["effect"].is<const char *>()) {
    int id = modeIdByName(doc["effect"]);
    if (id >= 0 && (uint8_t)id != modeId) setMode((uint8_t)id);
  }
  publishLightState();
}

void handleParamCommand(const String &key, const String &value) {
  const Mode &m = MODES[modeId];
  for (uint8_t i = 0; i < m.count; i++) {
    if (key == m.params[i].key) {
      long v = value.toInt();
      v = constrain(v, m.params[i].min, m.params[i].max);
      paramValues[i] = (uint16_t)v;
      publishParamState(i);
      return;
    }
  }
}

void onMessage(char *topic, byte *payload, unsigned int length) {
  String msg;
  msg.reserve(length);
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

  String t(topic);
  if (t == lightCmdTopic) {
    handleLightCommand(msg);
    return;
  }
  String prefix = baseTopic + "/param/";
  if (t.startsWith(prefix) && t.endsWith("/set")) {
    String key = t.substring(prefix.length(), t.length() - 4);  // strip "/set"
    handleParamCommand(key, msg);
  }
}

void connectMqtt() {
  mqtt.setBufferSize(MQTT_BUFFER_SIZE);
  mqtt.setServer(mqttHost.c_str(), mqttPort);
  mqtt.setCallback(onMessage);

  const char *user = mqttUser.length() ? mqttUser.c_str() : nullptr;
  const char *pass = mqttPass.length() ? mqttPass.c_str() : nullptr;

  while (!mqtt.connected()) {
    Serial.printf("[mqtt] connecting to %s:%u ...\n", mqttHost.c_str(), mqttPort);
    // LWT: broker publishes "offline" to availTopic on ungraceful disconnect.
    if (mqtt.connect(deviceId.c_str(), user, pass, availTopic.c_str(), 1, true,
                     "offline")) {
      Serial.println("[mqtt] connected");
      mqtt.publish(availTopic.c_str(), "online", true);
      publishLightDiscovery();
      publishParamDiscovery();
      publishLightState();
      publishParamStates();
      mqtt.subscribe(lightCmdTopic.c_str());
      mqtt.subscribe((baseTopic + "/param/+/set").c_str());
    } else {
      Serial.printf("[mqtt] failed rc=%d, retrying in 3s\n", mqtt.state());
      delay(3000);
    }
  }
}

// ---- Serial input -----------------------------------------------------------
bool pressedY() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'y' || c == 'Y') return true;
  }
  return false;
}

// ---- Arduino lifecycle ------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  pinMode(LED_PIN, OUTPUT);
  applyLight(false);

  // Seed the current mode's params with their defaults.
  for (uint8_t i = 0; i < MODES[modeId].count; i++)
    paramValues[i] = MODES[modeId].params[i].def;

  Serial.printf("[wifi] connecting to %s ...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\n[wifi] connected, ip=%s\n", WiFi.localIP().toString().c_str());

  macHex = macToHex();
  buildTopics();
  startWebServer();

  provisioned = loadCreds();
  if (provisioned) {
    Serial.println("[boot] credentials found, connecting to MQTT");
    connectMqtt();
  } else {
    printEnablePrompt();
  }
}

void loop() {
  if (provisioned) {
    if (!mqtt.connected()) connectMqtt();
    mqtt.loop();
    server.handleClient();  // keep /deprovision reachable
    return;
  }

  if (pairing) {
    server.handleClient();
    return;
  }

  // Idle: waiting for the user to enable discovery.
  if (pressedY()) startPairing();
}
