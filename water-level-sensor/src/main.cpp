/*
  Water Level Sensor
  Hardware: Wemos D1 Mini (ESP8266) + HC-SR04
  Features: WiFi AP/STA, MQTT, Telegram, OTA, Web UI, History chart
  v1.0.0
*/

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <LittleFS.h>
#include <time.h>
#include <functional>

#include "config.h"
#include "sensor.h"
#include "storage.h"
#include "mqtt_handler.h"
#include "telegram_handler.h"
#include "webserver.h"

// ── Globals ──────────────────────────────────────────────────────────────────
Config     cfg;
SensorData sens;
ESP8266WebServer    webServer(80);
ESP8266HTTPUpdateServer updater;

bool apMode = false;

// Timers
unsigned long tMeasure    = 0;
unsigned long tHourly     = 0;
unsigned long tTelegram   = 0;
unsigned long tMqtt       = 0;
unsigned long tMdns       = 0;

// Daily summary
int lastSummaryDay = -1;
String serialLine;

void doMeasureCallback();

// ── Serial console helpers ───────────────────────────────────────────────────
static String _trimQuotes(String s) {
  s.trim();
  if (s.length() >= 2) {
    char a = s.charAt(0);
    char b = s.charAt(s.length() - 1);
    if ((a == '"' && b == '"') || (a == '\'' && b == '\'')) {
      return s.substring(1, s.length() - 1);
    }
  }
  return s;
}

static bool _parseBool(const String &raw, bool &out) {
  String v = raw; v.toLowerCase(); v.trim();
  if (v == "1" || v == "true" || v == "on" || v == "yes") { out = true;  return true; }
  if (v == "0" || v == "false" || v == "off" || v == "no") { out = false; return true; }
  return false;
}

static void serialHelp() {
  Serial.println(F("[SER] Commands:"));
  Serial.println(F("  help"));
  Serial.println(F("  cfg show"));
  Serial.println(F("  cfg defaults"));
  Serial.println(F("  cfg save"));
  Serial.println(F("  cfg reload"));
  Serial.println(F("  cfg set <key> <value>"));
  Serial.println(F("  measure"));
  Serial.println(F("  wifi scan"));
  Serial.println(F("  reboot"));
  Serial.println(F("[SER] Examples:"));
  Serial.println(F("  cfg set tp 14"));
  Serial.println(F("  cfg set ep 12"));
  Serial.println(F("  cfg set dp 2"));
  Serial.println(F("  cfg set de true"));
  Serial.println(F("  cfg set ws MyWiFi"));
  Serial.println(F("  cfg set wp mypass"));
  Serial.println(F("  cfg set ms 60"));
  Serial.println(F("  cfg save"));
}

static bool serialSetConfig(const String &keyRaw, String value) {
  String key = keyRaw; key.toLowerCase(); key.trim();
  value = _trimQuotes(value);
  bool bv;

  // Strings
  if (key == "ws") { strlcpy(cfg.wifi_ssid, value.c_str(), sizeof(cfg.wifi_ssid)); return true; }
  if (key == "wp") { strlcpy(cfg.wifi_password, value.c_str(), sizeof(cfg.wifi_password)); return true; }
  if (key == "mh") { strlcpy(cfg.mqtt_host, value.c_str(), sizeof(cfg.mqtt_host)); return true; }
  if (key == "mu") { strlcpy(cfg.mqtt_user, value.c_str(), sizeof(cfg.mqtt_user)); return true; }
  if (key == "mq") { strlcpy(cfg.mqtt_pass, value.c_str(), sizeof(cfg.mqtt_pass)); return true; }
  if (key == "mt") { strlcpy(cfg.mqtt_topic, value.c_str(), sizeof(cfg.mqtt_topic)); return true; }
  if (key == "tt") { strlcpy(cfg.tg_token, value.c_str(), sizeof(cfg.tg_token)); return true; }
  if (key == "tc") { strlcpy(cfg.tg_chat, value.c_str(), sizeof(cfg.tg_chat)); return true; }
  if (key == "dn") { strlcpy(cfg.device_name, value.c_str(), sizeof(cfg.device_name)); return true; }
  if (key == "op") { strlcpy(cfg.ota_pass, value.c_str(), sizeof(cfg.ota_pass)); return true; }

  // Booleans
  if (key == "me") { if (!_parseBool(value, bv)) return false; cfg.mqtt_en = bv; return true; }
  if (key == "te") { if (!_parseBool(value, bv)) return false; cfg.tg_en = bv; return true; }
  if (key == "td") { if (!_parseBool(value, bv)) return false; cfg.tg_daily = bv; return true; }
  if (key == "de") { if (!_parseBool(value, bv)) return false; cfg.ds18_en = bv; return true; }

  // Integers
  if (key == "tp") { cfg.trig_pin = (uint8_t)value.toInt(); return true; }
  if (key == "ep") { cfg.echo_pin = (uint8_t)value.toInt(); return true; }
  if (key == "dp") { cfg.ds18_pin = (uint8_t)value.toInt(); return true; }
  if (key == "as") { cfg.avg_samples = (uint8_t)value.toInt(); return true; }
  if (key == "ms") { cfg.measure_sec = (uint16_t)value.toInt(); return true; }
  if (key == "mp") { cfg.mqtt_port = (uint16_t)value.toInt(); return true; }

  // Floats
  if (key == "ed") { cfg.empty_dist_cm = value.toFloat(); return true; }
  if (key == "fd") { cfg.full_dist_cm  = value.toFloat(); return true; }
  if (key == "bd") { cfg.barrel_diam_cm = value.toFloat(); return true; }
  if (key == "tl") { cfg.tg_alert_low = value.toFloat(); return true; }
  if (key == "th") { cfg.tg_alert_high = value.toFloat(); return true; }

  return false;
}

static void serialHandleCommand(String line) {
  line.trim();
  if (!line.length()) return;
  Serial.printf("[SER] > %s\n", line.c_str());

  if (line == "help") { serialHelp(); return; }
  if (line == "cfg show") { logConfigSummary("serial", cfg); return; }
  if (line == "cfg defaults") {
    configDefaults(cfg);
    Serial.println(F("[SER] Defaults loaded into RAM (use 'cfg save' to persist)"));
    logConfigSummary("serial", cfg);
    return;
  }
  if (line == "cfg save") {
    bool ok = saveConfig(cfg);
    Serial.printf("[SER] cfg save -> %s\n", ok ? "ok" : "fail");
    return;
  }
  if (line == "cfg reload") {
    bool ok = loadConfig(cfg);
    Serial.printf("[SER] cfg reload -> %s\n", ok ? "ok" : "defaults");
    return;
  }
  if (line == "measure") {
    doMeasureCallback();
    return;
  }
  if (line == "wifi scan") {
    String json = buildWifiScan();
    Serial.printf("[SER] wifi scan result: %s\n", json.c_str());
    return;
  }
  if (line == "reboot") {
    Serial.println(F("[SER] Rebooting..."));
    delay(100);
    ESP.restart();
    return;
  }

  if (line.startsWith("cfg set ")) {
    String rest = line.substring(8);
    rest.trim();
    int sp = rest.indexOf(' ');
    if (sp <= 0) {
      Serial.println(F("[SER] Usage: cfg set <key> <value>"));
      return;
    }
    String key = rest.substring(0, sp);
    String val = rest.substring(sp + 1);
    val.trim();
    if (!val.length()) {
      Serial.println(F("[SER] Empty value"));
      return;
    }
    if (!serialSetConfig(key, val)) {
      Serial.printf("[SER] Unknown key or invalid value: %s\n", key.c_str());
      return;
    }
    Serial.printf("[SER] cfg set %s ok\n", key.c_str());
    return;
  }

  Serial.println(F("[SER] Unknown command. Type 'help'"));
}

static void serialPoll() {
  while (Serial.available() > 0) {
    char ch = (char)Serial.read();
    if (ch == '\r') continue;
    if (ch == '\n') {
      serialHandleCommand(serialLine);
      serialLine = "";
      continue;
    }
    if (serialLine.length() < 255) serialLine += ch;
  }
}

// ── WiFi helpers ─────────────────────────────────────────────────────────────
bool connectWiFi() {
  if (!strlen(cfg.wifi_ssid)) return false;
  Serial.printf("[WiFi] Connecting to %s", cfg.wifi_ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.wifi_ssid, cfg.wifi_password);
  for (int i = 0; i < 40; i++) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("\n[WiFi] IP: %s\n", WiFi.localIP().toString().c_str());
      return true;
    }
    delay(500); Serial.print('.'); yield();
  }
  Serial.println(F("\n[WiFi] Failed"));
  return false;
}

void startAP() {
  apMode = true;
  String ssid = String(F("WaterSensor-")) + String(ESP.getChipId(), HEX);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid.c_str(), "watersensor");
  Serial.printf("[AP] SSID: %s  IP: %s\n", ssid.c_str(),
                WiFi.softAPIP().toString().c_str());
}

// ── NTP ──────────────────────────────────────────────────────────────────────
void setupNTP() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  // Wait for sync (max 5s)
  time_t t = 0;
  for (int i = 0; i < 10 && t < 1000000; i++) { delay(500); t = time(nullptr); }
  Serial.printf("[NTP] Time: %lu\n", (unsigned long)t);
}

// ── OTA ──────────────────────────────────────────────────────────────────────
void setupArduinoOTA() {
  ArduinoOTA.setHostname(cfg.device_name);
  if (strlen(cfg.ota_pass)) ArduinoOTA.setPassword(cfg.ota_pass);
  ArduinoOTA.onStart([]{ Serial.println(F("[OTA] Start")); });
  ArduinoOTA.onEnd([]{ Serial.println(F("\n[OTA] End")); });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int t){
    Serial.printf("[OTA] %u%%\r", p * 100 / t);
  });
  ArduinoOTA.onError([](ota_error_t e){
    Serial.printf("[OTA] Error %u\n", e);
  });
  ArduinoOTA.begin();
}

// ── Measure callback ──────────────────────────────────────────────────────────
void doMeasureCallback() {
  doMeasure(cfg, sens);
  Serial.printf("[Sensor] dist=%.1f cm  level=%.1f%%  vol=%.1f L  temp=%.1f°C\n",
                sens.distance_cm, sens.level_pct, sens.volume_liters, sens.temp_c);
  tgCheckAlerts(cfg, sens);
}

// ── setup ────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println(F("\n\n===== Water Level Sensor v1.0.0 ====="));
  Serial.println(F("[SER] Console ready. Type 'help'"));

  // LittleFS
  if (!LittleFS.begin()) {
    Serial.println(F("[FS] Format..."));
    LittleFS.format();
    LittleFS.begin();
  }

  // Config
  if (!loadConfig(cfg)) {
    Serial.println(F("[CFG] Using defaults"));
    saveConfig(cfg);
  }

  // Sensor
  initSensor(cfg);
  initTempSensor(cfg);
  storageInit();

  // WiFi
  if (!connectWiFi()) {
    startAP();
  } else {
    setupNTP();
    setupArduinoOTA();
    mqttSetup(cfg);
    tgSetup(cfg);
  }

  // mDNS
  if (MDNS.begin(cfg.device_name)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("[mDNS] http://%s.local\n", cfg.device_name);
  }

  // Web server
  webSetup(webServer, updater, cfg, sens, doMeasureCallback);
  webServer.begin();
  Serial.println(F("[HTTP] Server started"));

  // First measurement
  doMeasureCallback();
  tMeasure = millis();

  // Store first point in history right away
  if (!apMode) {
    storageWrite(sens);
    tHourly = millis();
  }
}

// ── loop ─────────────────────────────────────────────────────────────────────
void loop() {
  serialPoll();
  webServer.handleClient();
  MDNS.update();

  if (!apMode) {
    ArduinoOTA.handle();

    unsigned long now = millis();

    // Periodic measurement
    if (now - tMeasure >= (unsigned long)cfg.measure_sec * 1000UL) {
      doMeasureCallback();
      mqttPublish(cfg, sens);
      tMeasure = now;
    }

    // Hourly snapshot for history
    if (now - tHourly >= 3600000UL) {
      storageWrite(sens);
      tHourly = now;
    }

    // MQTT keep-alive + auto-discovery
    if (now - tMqtt >= 1000UL) {
      mqttLoop(cfg);
      if (mqttConnected()) mqttDiscovery(cfg);  // no-op after first send
      tMqtt = now;
    }

    // Telegram polling
    if (cfg.tg_en && now - tTelegram >= 10000UL) {
      tgLoop(cfg, sens);
      tTelegram = now;
    }

    // Daily summary at midnight
    if (cfg.tg_en && cfg.tg_daily) {
      time_t t = time(nullptr);
      struct tm *ti = localtime(&t);
      if (ti->tm_hour == 0 && ti->tm_min == 0 && ti->tm_mday != lastSummaryDay) {
        tgDailySummary(cfg, sens);
        lastSummaryDay = ti->tm_mday;
      }
    }

    // WiFi watchdog
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println(F("[WiFi] Reconnecting..."));
      WiFi.reconnect();
      delay(5000);
    }
  }

}
