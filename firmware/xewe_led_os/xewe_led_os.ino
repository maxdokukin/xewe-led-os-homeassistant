/*
 * XeWe LED dock — sample ESP32 firmware
 * -----------------------------------------
 * Minimal device that Home Assistant can auto-discover over mDNS/zeroconf and
 * reach over HTTP. No MQTT, no broker, no pairing step.
 *
 *   1. Connects to Wi-Fi using the credentials in wifi.h.
 *   2. Advertises an mDNS service _xewe-led-os._tcp.local on port 80, with a
 *      TXT record carrying the device MAC (used by HA as the unique id) and the
 *      firmware version.
 *   3. Serves HTTP on port 80. GET / answers with a small JSON status blob; the
 *      HA integration treats any response here as "device is reachable".
 *
 * The real LED control API (brightness / color / effects) is intentionally not
 * implemented yet — this sketch only proves discovery + reachability, which is
 * exactly what the current HA integration exercises.
 *
 * Required Arduino libraries: none beyond the ESP32 core (WiFi.h, ESPmDNS.h,
 * WebServer.h are all bundled with it).
 */

#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>

// ---- User configuration -----------------------------------------------------
// Wi-Fi credentials live in wifi.h (gitignored). Copy wifi.h.example to wifi.h
// and fill in WIFI_SSID / WIFI_PASS.
#include "wifi_c.h"

#define LED_PIN 8               // GPIO the onboard LED/relay is wired to
#define LED_ACTIVE_HIGH true    // set false if your LED/relay is active-low
#define FW_VERSION "0.2.0"

// ---- Globals ----------------------------------------------------------------
WebServer server(80);

String deviceId;   // "xewe_led_os_aabbccddeeff"
String macHex;     // "aabbccddeeff"

// ---- Helpers ----------------------------------------------------------------
String macToHex() {
  String m = WiFi.macAddress();  // "AA:BB:CC:DD:EE:FF"
  m.replace(":", "");
  m.toLowerCase();
  return m;
}

// ---- HTTP -------------------------------------------------------------------
// GET / — reachability probe + basic identity. HA's config flow and the
// "Identify" button both just check that this responds.
void handleRoot() {
  String body = "{\"id\":\"" + deviceId + "\",\"mac\":\"" + macHex +
                "\",\"fw\":\"" FW_VERSION "\"}";
  server.send(200, "application/json", body);
}

void startWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.begin();
}

// ---- mDNS -------------------------------------------------------------------
void startMdns() {
  String host = "xewe-led-os-" + macHex.substring(6);
  if (!MDNS.begin(host.c_str())) {
    Serial.println("[mdns] failed to start");
    return;
  }
  MDNS.addService("_xewe-led-os", "_tcp", 80);
  MDNS.addServiceTxt("_xewe-led-os", "_tcp", "mac", macHex);
  MDNS.addServiceTxt("_xewe-led-os", "_tcp", "fw", FW_VERSION);
  Serial.printf("[mdns] advertising %s._xewe-led-os._tcp.local (%s)\n",
                host.c_str(), WiFi.localIP().toString().c_str());
}

// ---- Arduino lifecycle ------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LED_ACTIVE_HIGH ? LOW : HIGH);  // start off

  Serial.printf("[wifi] connecting to %s ...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\n[wifi] connected, ip=%s\n", WiFi.localIP().toString().c_str());

  macHex = macToHex();
  deviceId = "xewe_led_os_" + macHex;

  startWebServer();
  startMdns();

  Serial.println("[boot] ready — discoverable in Home Assistant");
}

void loop() {
  server.handleClient();
}
