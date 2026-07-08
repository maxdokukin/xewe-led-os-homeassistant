/*
 * XEWE LED-OS dock — sample ESP32 firmware
 * -----------------------------------------
 * Minimal on/off switch on a single GPIO, integrated with Home Assistant via
 * MQTT auto-discovery, with zero-typing "auto-pair":
 *
 *   1. First boot with no Wi-Fi -> WiFiManager captive portal (AP "XEWE-Dock-XXXX").
 *   2. Once on Wi-Fi and not yet provisioned -> PAIRING MODE:
 *        - advertise mDNS service _xewe-led._tcp.local (TXT: mac, fw)
 *        - print a 6-digit PIN on Serial
 *        - accept POST /provision {host,port,user,pass,pin} during a time window
 *   3. On valid provision -> store broker creds in NVS, connect to MQTT,
 *        publish a RETAINED switch discovery config, and drive SWITCH_PIN.
 *
 * Required Arduino libraries (Library Manager):
 *   - WiFiManager (tzapu)
 *   - PubSubClient (knolleary)
 *   - ArduinoJson (bblanchon)
 * Bundled with the ESP32 core: WiFi.h, ESPmDNS.h, WebServer.h, Preferences.h
 */

#include <WiFi.h>
#include <WiFiManager.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// ---- User configuration -----------------------------------------------------
#define SWITCH_PIN 26            // GPIO the relay/LED is wired to
#define SWITCH_ACTIVE_HIGH true  // set false if your relay is active-low
#define FW_VERSION "0.1.0"
#define AP_PREFIX "XEWE-Dock-"
#define PAIRING_WINDOW_MS 120000UL  // PIN validity per cycle (2 min)
#define MQTT_BUFFER_SIZE 1024       // MUST exceed the discovery JSON size

// ---- Globals ----------------------------------------------------------------
Preferences prefs;
WiFiClient net;
PubSubClient mqtt(net);
WebServer server(80);

String deviceId;    // "xewe_dock_aabbccddeeff"
String macHex;      // "aabbccddeeff"
String baseTopic;   // "xewe/<deviceId>"
String cmdTopic, stateTopic, availTopic, discoveryTopic;

String mqttHost, mqttUser, mqttPass;
uint16_t mqttPort = 1883;

bool provisioned = false;
bool pairing = false;
unsigned long pairingStart = 0;
char pin[7] = {0};
bool switchState = false;

// ---- Helpers ----------------------------------------------------------------
String macToHex() {
  String m = WiFi.macAddress();  // "AA:BB:CC:DD:EE:FF"
  m.replace(":", "");
  m.toLowerCase();
  return m;
}

void buildTopics() {
  deviceId = "xewe_dock_" + macHex;
  baseTopic = "xewe/" + deviceId;
  cmdTopic = baseTopic + "/cmd";
  stateTopic = baseTopic + "/state";
  availTopic = baseTopic + "/avail";
  discoveryTopic = "homeassistant/switch/" + deviceId + "/config";
}

void applySwitch(bool on) {
  switchState = on;
  digitalWrite(SWITCH_PIN, (on == SWITCH_ACTIVE_HIGH) ? HIGH : LOW);
}

void generatePin() {
  snprintf(pin, sizeof(pin), "%06u", (unsigned)(esp_random() % 1000000UL));
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

// ---- Pairing (mDNS + /provision) --------------------------------------------
void handleProvision() {
  if (!pairing || (millis() - pairingStart) > PAIRING_WINDOW_MS) {
    server.send(403, "application/json", "{\"error\":\"not_pairing\"}");
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"error\":\"bad_json\"}");
    return;
  }
  if (strcmp(doc["pin"] | "", pin) != 0) {
    server.send(403, "application/json", "{\"error\":\"invalid_pin\"}");
    return;
  }

  saveCreds(doc["host"] | "", doc["port"] | 1883, doc["user"] | "",
            doc["pass"] | "");
  server.send(200, "application/json", "{\"ok\":true}");
  Serial.println("[pair] provisioned; leaving pairing mode");

  provisioned = true;
  pairing = false;
  server.stop();
  MDNS.end();
}

void startPairing() {
  generatePin();
  pairing = true;
  pairingStart = millis();

  String host = "xewe-dock-" + macHex.substring(6);
  if (MDNS.begin(host.c_str())) {
    MDNS.addService("_xewe-led", "_tcp", 80);
    MDNS.addServiceTxt("_xewe-led", "_tcp", "mac", macHex);
    MDNS.addServiceTxt("_xewe-led", "_tcp", "fw", FW_VERSION);
  }
  server.on("/provision", HTTP_POST, handleProvision);
  server.begin();

  Serial.println("========================================");
  Serial.printf("[pair] PAIRING MODE — enter this PIN in HA: %s\n", pin);
  Serial.printf("[pair] mDNS: %s._xewe-led._tcp.local  (%s)\n",
                host.c_str(), WiFi.localIP().toString().c_str());
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
  dev["name"] = "XEWE Dock";
  dev["manufacturer"] = "XEWE";
  dev["model"] = "LED-OS Dock";
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

// ---- Arduino lifecycle ------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  pinMode(SWITCH_PIN, OUTPUT);
  applySwitch(false);

  WiFiManager wm;
  macHex = macToHex();
  String apName = String(AP_PREFIX) + macHex.substring(6);
  Serial.printf("[wifi] connecting (portal AP: %s if needed)\n", apName.c_str());
  if (!wm.autoConnect(apName.c_str())) {
    Serial.println("[wifi] portal timed out, restarting");
    ESP.restart();
  }
  Serial.printf("[wifi] connected, ip=%s\n", WiFi.localIP().toString().c_str());

  macHex = macToHex();  // MAC is stable now
  buildTopics();

  provisioned = loadCreds();
  if (provisioned) {
    Serial.println("[boot] credentials found, connecting to MQTT");
    connectMqtt();
  } else {
    startPairing();
  }
}

void loop() {
  if (pairing) {
    server.handleClient();
    if ((millis() - pairingStart) > PAIRING_WINDOW_MS) {
      // Refresh the PIN so a captured one expires, and keep pairing usable.
      generatePin();
      pairingStart = millis();
      Serial.printf("[pair] PIN expired, new PIN: %s\n", pin);
    }
    return;
  }

  if (provisioned) {
    if (!mqtt.connected()) connectMqtt();
    mqtt.loop();
  }
}
