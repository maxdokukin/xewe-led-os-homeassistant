/*
 * XeWe LED dock — sample ESP32 firmware
 * -----------------------------------------
 * Minimal on/off switch on a single GPIO, integrated with Home Assistant via
 * MQTT auto-discovery.
 *
 *   1. Connects to Wi-Fi using the credentials defined below.
 *   2. If not yet provisioned, prints a prompt on Serial. Press 'y' to enter
 *      DISCOVERY MODE, which stays active until the device is provisioned:
 *        - advertise mDNS service _xewe-led-os._tcp.local (TXT: mac, fw)
 *        - accept POST /provision {host,port,user,pass} (no PIN)
 *   3. On provision -> store broker creds in NVS, connect to MQTT, publish a
 *      RETAINED switch discovery config, and drive SWITCH_PIN.
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
#define WIFI_SSID "fuck villa torino"
#define WIFI_PASS "xyrrAw-vytsed-votsi3"

#define SWITCH_PIN 8            // GPIO the relay/LED is wired to
#define SWITCH_ACTIVE_HIGH true  // set false if your relay is active-low
#define FW_VERSION "0.1.0"
#define MQTT_BUFFER_SIZE 1024         // MUST exceed the discovery JSON size

// ---- Globals ----------------------------------------------------------------
Preferences prefs;
WiFiClient net;
PubSubClient mqtt(net);
WebServer server(80);

String deviceId;    // "xewe_led_os_aabbccddeeff"
String macHex;      // "aabbccddeeff"
String baseTopic;   // "xewe_led_os/<deviceId>"
String cmdTopic, stateTopic, availTopic, discoveryTopic;

String mqttHost, mqttUser, mqttPass;
uint16_t mqttPort = 1883;

bool provisioned = false;
bool pairing = false;
bool switchState = false;

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
  cmdTopic = baseTopic + "/cmd";
  stateTopic = baseTopic + "/state";
  availTopic = baseTopic + "/avail";
  discoveryTopic = "homeassistant/switch/" + deviceId + "/config";
}

void applySwitch(bool on) {
  switchState = on;
  digitalWrite(SWITCH_PIN, (on == SWITCH_ACTIVE_HIGH) ? HIGH : LOW);
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

// Factory reset: clear the retained MQTT discovery so HA drops the entity, wipe
// stored broker creds, and reboot back into discovery mode. This is what makes
// "delete the device in Home Assistant" actually release the hardware instead of
// leaving a ghost behind.
void handleDeprovision() {
  server.send(200, "application/json", "{\"ok\":true}");
  Serial.println("[deprovision] clearing broker creds + retained discovery");

  if (mqtt.connected()) {
    mqtt.publish(discoveryTopic.c_str(), "", true);  // empty retained = delete
    mqtt.publish(stateTopic.c_str(), "", true);
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

// ---- MQTT -------------------------------------------------------------------
void publishState() {
  mqtt.publish(stateTopic.c_str(), switchState ? "ON" : "OFF", true);
}

void publishDiscovery() {
  JsonDocument doc;
  doc["~"] = baseTopic;
  doc["name"] = "Dock Relay";
  doc["unique_id"] = deviceId + "_switch";
  doc["command_topic"] = "~/cmd";
  doc["state_topic"] = "~/state";
  doc["availability_topic"] = "~/avail";
  doc["payload_on"] = "ON";
  doc["payload_off"] = "OFF";
  doc["payload_available"] = "online";
  doc["payload_not_available"] = "offline";
  JsonObject dev = doc["device"].to<JsonObject>();
  dev["identifiers"][0] = deviceId;
  dev["name"] = "XeWe LED";
  dev["manufacturer"] = "XeWe";
  dev["model"] = "LED Dock";
  dev["sw_version"] = FW_VERSION;

  String payload;
  serializeJson(doc, payload);
  bool ok = mqtt.publish(discoveryTopic.c_str(), payload.c_str(), true);
  Serial.printf("[mqtt] discovery published (%u bytes): %s\n", payload.length(),
                ok ? "ok" : "FAILED (raise MQTT_BUFFER_SIZE?)");
}

void onMessage(char *topic, byte *payload, unsigned int length) {
  String msg;
  msg.reserve(length);
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  if (msg == "ON") applySwitch(true);
  else if (msg == "OFF") applySwitch(false);
  publishState();
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
      publishDiscovery();
      mqtt.subscribe(cmdTopic.c_str());
      publishState();
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
  pinMode(SWITCH_PIN, OUTPUT);
  applySwitch(false);

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
