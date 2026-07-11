/*
 * XeWe LED dock — sample ESP32 firmware (mock data model)
 * -------------------------------------------------------
 * Relays a mock LED data model to Home Assistant over MQTT auto-discovery,
 * mirroring how the real device's WebInterface models things:
 *
 *   - a device NAME (carried by the shared MQTT `device` block),
 *   - on/off STATE, BRIGHTNESS (0-255) and COLOR (HS) on a JSON-schema MQTT
 *     `light`. State/brightness/color are mode-agnostic — always present.
 *     Saturation defaults to full, so the color picker acts as a hue control.
 *   - the active MODE as a plain integer (mock modes: Solid, Fade, Rainbow),
 *     exposed to HA as a `select` dropdown whose options are the mode names,
 *   - per-mode PARAMS as MQTT `number` entities: Solid none, Fade speed+depth,
 *     Rainbow speed. Only the current mode's params are shown — on mode change
 *     the firmware publishes that mode's params and clears the rest.
 *
 * Modes and params are a STATIC compile-time list (like the reference's mode
 * registry); selecting a mode just moves an integer.
 *
 * Provisioning is unchanged from the on/off sample:
 *   1. Connects to Wi-Fi using the credentials in wifi_c.h.
 *   2. If not yet provisioned, press 'y' on Serial to enter DISCOVERY MODE:
 *        - advertise mDNS service _xewe-led-os._tcp.local (TXT: mac, fw)
 *        - accept POST /provision {host,port,user,pass} (no PIN)
 *   3. On provision -> store broker creds in NVS, connect to MQTT, publish the
 *      RETAINED discovery config for the light + mode select.
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
#define MQTT_BUFFER_SIZE 1024   // MUST exceed the discovery JSON size

// ---- Mock data model --------------------------------------------------------
// Static list of modes (mirrors the reference device's mode registry). Mode is
// just an index into this array.
static const char *MODE_NAMES[] = {"Solid", "Fade", "Rainbow"};
static const uint8_t MODE_COUNT = sizeof(MODE_NAMES) / sizeof(MODE_NAMES[0]);

// The full, fixed set of params. Each mode uses a subset (see modeHasParam).
// `value` is the current mutable value; the rest describe the HA number entity.
struct Param {
  const char *key;
  const char *name;
  uint16_t min;
  uint16_t max;
  uint16_t step;
  uint16_t def;
  uint16_t value;
};
Param PARAMS[] = {
    {"speed", "Speed", 1, 100, 1, 50, 50},
    {"depth", "Depth", 1, 100, 1, 50, 50},
};
static const uint8_t PARAM_COUNT = sizeof(PARAMS) / sizeof(PARAMS[0]);

// Which params each mode exposes: Solid none, Fade speed+depth, Rainbow speed.
bool modeHasParam(uint8_t mode, const char *key) {
  bool isSpeed = strcmp(key, "speed") == 0;
  bool isDepth = strcmp(key, "depth") == 0;
  switch (mode) {
    case 1: return isSpeed || isDepth;  // Fade
    case 2: return isSpeed;             // Rainbow
    default: return false;             // Solid (and unknown)
  }
}

// ---- Globals ----------------------------------------------------------------
Preferences prefs;
WiFiClient net;
PubSubClient mqtt(net);
WebServer server(80);

String deviceId;    // "xewe_led_os_aabbccddeeff"
String macHex;      // "aabbccddeeff"
String baseTopic;   // "xewe_led_os/<deviceId>"
String availTopic, lightCmdTopic, lightStateTopic, lightDiscoveryTopic;
String modeCmdTopic, modeStateTopic, modeDiscoveryTopic;

String mqttHost, mqttUser, mqttPass;
uint16_t mqttPort = 1883;

bool provisioned = false;
bool pairing = false;

// Mock device state.
String deviceName = "XeWe LED";
bool lightOn = false;
uint8_t brightness = 255;
float hue = 0.0f;      // 0-360
float sat = 100.0f;    // 0-100; defaults to full so color acts as a hue control
uint8_t modeId = 0;

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
  modeCmdTopic = baseTopic + "/mode/set";
  modeStateTopic = baseTopic + "/mode/state";
  modeDiscoveryTopic = "homeassistant/select/" + deviceId + "/config";
}

void applyLight(bool on) {
  lightOn = on;
  digitalWrite(LED_PIN, (on == LED_ACTIVE_HIGH) ? HIGH : LOW);
}

int modeIdByName(const char *name) {
  for (uint8_t i = 0; i < MODE_COUNT; i++)
    if (strcmp(MODE_NAMES[i], name) == 0) return i;
  return -1;
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

// Factory reset: clear the retained MQTT discovery so HA drops the entities
// (light + mode select), wipe stored broker creds, and reboot back into
// discovery mode. This is what makes "delete the device in Home Assistant"
// actually release the hardware instead of leaving a ghost behind.
void handleDeprovision() {
  server.send(200, "application/json", "{\"ok\":true}");
  Serial.println("[deprovision] clearing broker creds + retained discovery");

  if (mqtt.connected()) {
    // Empty retained payload deletes the retained message = HA drops the entity.
    mqtt.publish(lightDiscoveryTopic.c_str(), "", true);
    mqtt.publish(lightStateTopic.c_str(), "", true);
    mqtt.publish(modeDiscoveryTopic.c_str(), "", true);
    mqtt.publish(modeStateTopic.c_str(), "", true);
    for (uint8_t i = 0; i < PARAM_COUNT; i++) {
      mqtt.publish(paramDiscoveryTopic(PARAMS[i].key).c_str(), "", true);
      mqtt.publish(paramStateTopic(PARAMS[i].key).c_str(), "", true);
    }
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
// Shared device block so HA groups the light + mode select under one device.
// The device `name` is how the mock device name reaches HA.
void addDevice(JsonDocument &doc) {
  JsonObject dev = doc["device"].to<JsonObject>();
  dev["identifiers"][0] = deviceId;
  dev["name"] = deviceName;
  dev["manufacturer"] = "XeWe";
  dev["model"] = "LED";
  dev["sw_version"] = FW_VERSION;
}

// One JSON-schema light carrying state + brightness + HS color.
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
  doc["color_mode"] = true;
  doc["supported_color_modes"][0] = "hs";
  addDevice(doc);

  String payload;
  serializeJson(doc, payload);
  bool ok = mqtt.publish(lightDiscoveryTopic.c_str(), payload.c_str(), true);
  Serial.printf("[mqtt] light discovery (%u bytes): %s\n", payload.length(),
                ok ? "ok" : "FAILED (raise MQTT_BUFFER_SIZE?)");
}

// A `select` (dropdown) for the active mode; options = the mode names.
void publishModeDiscovery() {
  JsonDocument doc;
  doc["~"] = baseTopic;
  doc["name"] = "Mode";
  doc["unique_id"] = deviceId + "_mode";
  doc["command_topic"] = "~/mode/set";
  doc["state_topic"] = "~/mode/state";
  doc["availability_topic"] = "~/avail";
  JsonArray opts = doc["options"].to<JsonArray>();
  for (uint8_t i = 0; i < MODE_COUNT; i++) opts.add(MODE_NAMES[i]);
  addDevice(doc);

  String payload;
  serializeJson(doc, payload);
  mqtt.publish(modeDiscoveryTopic.c_str(), payload.c_str(), true);
}

// A `number` entity for one param of the current mode.
void publishParamDiscovery(const Param &p) {
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
  addDevice(doc);

  String payload;
  serializeJson(doc, payload);
  mqtt.publish(paramDiscoveryTopic(p.key).c_str(), payload.c_str(), true);
}

// ---- MQTT: state ------------------------------------------------------------
// Publish state. Call these on EVERY state change, not just on MQTT commands —
// a physical button / HomeKit / Alexa change must also be published here,
// otherwise Home Assistant won't see it (that push is the whole reason this
// firmware uses MQTT instead of HTTP polling).
void publishLightState() {
  JsonDocument doc;
  doc["state"] = lightOn ? "ON" : "OFF";
  doc["brightness"] = brightness;
  doc["color_mode"] = "hs";
  JsonObject color = doc["color"].to<JsonObject>();
  color["h"] = hue;
  color["s"] = sat;
  String payload;
  serializeJson(doc, payload);
  mqtt.publish(lightStateTopic.c_str(), payload.c_str(), true);
}

void publishModeState() {
  mqtt.publish(modeStateTopic.c_str(), MODE_NAMES[modeId], true);
}

void publishParamState(const Param &p) {
  mqtt.publish(paramStateTopic(p.key).c_str(), String(p.value).c_str(), true);
}

// Set a param (if it belongs to the current mode), clamped, and echo its state.
bool setParam(const char *key, long value) {
  for (uint8_t i = 0; i < PARAM_COUNT; i++) {
    Param &p = PARAMS[i];
    if (strcmp(p.key, key) == 0 && modeHasParam(modeId, key)) {
      p.value = (uint16_t)constrain(value, p.min, p.max);
      publishParamState(p);
      return true;
    }
  }
  return false;
}

// Publish the current mode's params and clear the rest, so HA only ever shows
// the controls that apply to the active mode. Stateless: it always reconciles
// every known param against the current mode (empty retained payload = delete).
void publishParams() {
  for (uint8_t i = 0; i < PARAM_COUNT; i++) {
    Param &p = PARAMS[i];
    if (modeHasParam(modeId, p.key)) {
      publishParamDiscovery(p);
      publishParamState(p);
    } else {
      mqtt.publish(paramDiscoveryTopic(p.key).c_str(), "", true);
      mqtt.publish(paramStateTopic(p.key).c_str(), "", true);
    }
  }
}

// Switch modes: reset params to defaults, publish the new mode's state + params.
void setMode(uint8_t id) {
  modeId = id;
  for (uint8_t i = 0; i < PARAM_COUNT; i++) PARAMS[i].value = PARAMS[i].def;
  publishModeState();
  publishParams();
}

// ---- Console output ---------------------------------------------------------
// Dump the full LED data model to Serial. Printed whenever HA pushes an update
// and whenever a local (Serial) command changes state.
void printState() {
  Serial.println("---- LED state ----");
  Serial.printf("  name       : %s\n", deviceName.c_str());
  Serial.printf("  power      : %s\n", lightOn ? "ON" : "OFF");
  Serial.printf("  brightness : %u\n", brightness);
  Serial.printf("  color      : hue=%.0f sat=%.0f\n", hue, sat);
  Serial.printf("  mode       : %u (%s)\n", modeId, MODE_NAMES[modeId]);
  for (uint8_t i = 0; i < PARAM_COUNT; i++) {
    if (modeHasParam(modeId, PARAMS[i].key))
      Serial.printf("  %-11s: %u  [%u..%u]\n", PARAMS[i].key, PARAMS[i].value,
                    PARAMS[i].min, PARAMS[i].max);
  }
  Serial.println("-------------------");
}

void printHelp() {
  Serial.println("Serial commands (push local changes to Home Assistant):");
  Serial.println("  on | off | toggle     turn the light on/off");
  Serial.println("  b <0-255>             set brightness");
  Serial.println("  color <hue> [sat]     set color (hue 0-360, sat 0-100)");
  Serial.println("  mode <id|name>        change mode");
  Serial.println("  set <key> <value>     set a param of the current mode");
  Serial.println("  name <text>           rename the device");
  Serial.println("  state                 print the current state");
  Serial.println("  help                  show this help");
  Serial.println("Modes:");
  for (uint8_t i = 0; i < MODE_COUNT; i++)
    Serial.printf("  %u = %s\n", i, MODE_NAMES[i]);
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
  if (doc["color"].is<JsonObject>()) {
    JsonObject c = doc["color"];
    if (!c["h"].isNull()) hue = constrain(c["h"].as<float>(), 0.0f, 360.0f);
    if (!c["s"].isNull()) sat = constrain(c["s"].as<float>(), 0.0f, 100.0f);
  }
  publishLightState();
}

void onMessage(char *topic, byte *payload, unsigned int length) {
  String msg;
  msg.reserve(length);
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

  String t(topic);
  if (t == lightCmdTopic) {
    handleLightCommand(msg);
    Serial.printf("[ha->dev] light command: %s\n", msg.c_str());
    printState();
    return;
  }
  if (t == modeCmdTopic) {
    int id = modeIdByName(msg.c_str());
    if (id >= 0) {
      setMode((uint8_t)id);
      Serial.printf("[ha->dev] mode = %s\n", MODE_NAMES[modeId]);
      printState();
    }
    return;
  }
  String prefix = baseTopic + "/param/";
  if (t.startsWith(prefix) && t.endsWith("/set")) {
    String key = t.substring(prefix.length(), t.length() - 4);  // strip "/set"
    if (setParam(key.c_str(), msg.toInt())) {
      Serial.printf("[ha->dev] param %s = %s\n", key.c_str(), msg.c_str());
      printState();
    }
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
      publishModeDiscovery();
      publishLightState();
      publishModeState();
      publishParams();
      mqtt.subscribe(lightCmdTopic.c_str());
      mqtt.subscribe(modeCmdTopic.c_str());
      mqtt.subscribe((baseTopic + "/param/+/set").c_str());
      Serial.println("[mqtt] ready. Type 'help' for Serial test commands.");
      printState();
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

// Non-blocking, line-buffered Serial reader. Returns true once a full line
// (terminated by CR/LF) is available in `out`.
String serialLine;
bool readSerialLine(String &out) {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialLine.length() == 0) return false;
      out = serialLine;
      serialLine = "";
      return true;
    }
    serialLine += c;
  }
  return false;
}

int parseMode(const String &s) {
  bool numeric = s.length() > 0;
  for (uint16_t i = 0; i < s.length(); i++)
    if (!isDigit(s[i])) numeric = false;
  if (numeric) {
    int id = s.toInt();
    return (id >= 0 && id < MODE_COUNT) ? id : -1;
  }
  for (uint8_t i = 0; i < MODE_COUNT; i++)
    if (s.equalsIgnoreCase(MODE_NAMES[i])) return i;
  return -1;
}

// Simulate a device-originated change (physical button / HomeKit / Alexa): apply
// it locally, publish to MQTT so it pushes to HA, then dump the new state.
void handleSerialCommand(const String &raw) {
  String line = raw;
  line.trim();
  if (line.length() == 0) return;

  int sp = line.indexOf(' ');
  String cmd = (sp < 0) ? line : line.substring(0, sp);
  String rest = (sp < 0) ? "" : line.substring(sp + 1);
  cmd.toLowerCase();
  rest.trim();

  if (cmd == "help" || cmd == "h" || cmd == "?") {
    printHelp();
    return;
  }
  if (cmd == "state" || cmd == "s") {
    printState();
    return;
  }
  if (cmd == "on" || cmd == "off" || cmd == "toggle" || cmd == "t") {
    applyLight(cmd == "on" ? true : cmd == "off" ? false : !lightOn);
    publishLightState();
  } else if (cmd == "b" || cmd == "bright") {
    brightness = (uint8_t)constrain(rest.toInt(), 0, 255);
    publishLightState();
  } else if (cmd == "color" || cmd == "c") {
    int s2 = rest.indexOf(' ');
    if (s2 < 0) {
      hue = constrain(rest.toFloat(), 0.0f, 360.0f);
    } else {
      hue = constrain(rest.substring(0, s2).toFloat(), 0.0f, 360.0f);
      sat = constrain(rest.substring(s2 + 1).toFloat(), 0.0f, 100.0f);
    }
    publishLightState();
  } else if (cmd == "mode" || cmd == "m") {
    int id = parseMode(rest);
    if (id < 0) {
      Serial.printf("[dev->ha] unknown mode '%s' (try 'help')\n", rest.c_str());
      return;
    }
    setMode((uint8_t)id);
  } else if (cmd == "set") {
    int s2 = rest.indexOf(' ');
    if (s2 < 0) {
      Serial.println("[dev->ha] usage: set <key> <value>");
      return;
    }
    String key = rest.substring(0, s2);
    String val = rest.substring(s2 + 1);
    val.trim();
    if (!setParam(key.c_str(), val.toInt())) {
      Serial.printf("[dev->ha] no param '%s' in mode '%s'\n", key.c_str(),
                    MODE_NAMES[modeId]);
      return;
    }
  } else if (cmd == "name") {
    if (rest.length() == 0) {
      Serial.println("[dev->ha] usage: name <text>");
      return;
    }
    deviceName = rest;
    // The device name rides the shared device block; republish discovery so HA
    // picks up the rename for the light, the mode select, and the params.
    publishLightDiscovery();
    publishModeDiscovery();
    publishParams();
  } else {
    Serial.printf("[dev->ha] unknown command '%s' (try 'help')\n", cmd.c_str());
    return;
  }

  Serial.println("[dev->ha] pushed update to Home Assistant:");
  printState();
}

// ---- Arduino lifecycle ------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  pinMode(LED_PIN, OUTPUT);
  applyLight(false);

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
    String line;
    if (readSerialLine(line)) handleSerialCommand(line);
    return;
  }

  if (pairing) {
    server.handleClient();
    return;
  }

  // Idle: waiting for the user to enable discovery.
  if (pressedY()) startPairing();
}
