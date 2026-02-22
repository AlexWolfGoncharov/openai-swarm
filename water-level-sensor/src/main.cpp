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
bool bootPhase = true;

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
  dbgPrintln(F("[SER] Commands:"));
  dbgPrintln(F("  help"));
  dbgPrintln(F("  cfg show"));
  dbgPrintln(F("  cfg defaults"));
  dbgPrintln(F("  cfg save"));
  dbgPrintln(F("  cfg reload"));
  dbgPrintln(F("  cfg raw"));
  dbgPrintln(F("  cfg set <key> <value>"));
  dbgPrintln(F("  measure"));
  dbgPrintln(F("  wifi scan"));
  dbgPrintln(F("  reboot"));
  dbgPrintln(F("[SER] Examples:"));
  dbgPrintln(F("  cfg set tp 14"));
  dbgPrintln(F("  cfg set ep 12"));
  dbgPrintln(F("  cfg set dp 2"));
  dbgPrintln(F("  cfg set de true"));
  dbgPrintln(F("  cfg set ws MyWiFi"));
  dbgPrintln(F("  cfg set wp mypass"));
  dbgPrintln(F("  cfg set ms 60"));
  dbgPrintln(F("  cfg save"));
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
  if (key == "tx") { if (!_parseBool(value, bv)) return false; cfg.tg_cmd_en = bv; return true; }
  if (key == "ta")  { if (!_parseBool(value, bv)) return false; cfg.tg_alert_low_en = bv; cfg.tg_alert_high_en = bv; return true; } // legacy alias
  if (key == "tal") { if (!_parseBool(value, bv)) return false; cfg.tg_alert_low_en = bv; return true; }
  if (key == "tah") { if (!_parseBool(value, bv)) return false; cfg.tg_alert_high_en = bv; return true; }
  if (key == "tb") { if (!_parseBool(value, bv)) return false; cfg.tg_boot_msg_en = bv; return true; }
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
  dbgPrintf("[SER] > %s\n", line.c_str());

  if (line == "help") { serialHelp(); return; }
  if (line == "cfg show") { logConfigSummary("serial", cfg); return; }
  if (line == "cfg defaults") {
    configDefaults(cfg);
    dbgPrintln(F("[SER] Defaults loaded into RAM (use 'cfg save' to persist)"));
    logConfigSummary("serial", cfg);
    return;
  }
  if (line == "cfg save") {
    bool ok = saveConfig(cfg);
    dbgPrintf("[SER] cfg save -> %s\n", ok ? "ok" : "fail");
    return;
  }
  if (line == "cfg reload") {
    bool ok = loadConfig(cfg);
    dbgPrintf("[SER] cfg reload -> %s\n", ok ? "ok" : "defaults");
    return;
  }
  if (line == "cfg raw") {
    File f = LittleFS.open(CONFIG_FILE, "r");
    if (!f) {
      dbgPrintln(F("[SER] cfg raw -> no /config.json"));
      return;
    }
    String raw = f.readString();
    f.close();
    dbgPrint(F("[SER] cfg raw: "));
    dbgPrintln(raw);
    return;
  }
  if (line == "measure") {
    doMeasureCallback();
    return;
  }
  if (line == "wifi scan") {
    String json = buildWifiScan();
    dbgPrintf("[SER] wifi scan result: %s\n", json.c_str());
    return;
  }
  if (line == "reboot") {
    dbgPrintln(F("[SER] Rebooting..."));
    delay(100);
    ESP.restart();
    return;
  }

  if (line.startsWith("cfg set ")) {
    String rest = line.substring(8);
    rest.trim();
    int sp = rest.indexOf(' ');
    if (sp <= 0) {
      dbgPrintln(F("[SER] Usage: cfg set <key> <value>"));
      return;
    }
    String key = rest.substring(0, sp);
    String val = rest.substring(sp + 1);
    val.trim();
    if (!val.length()) {
      dbgPrintln(F("[SER] Empty value"));
      return;
    }
    if (!serialSetConfig(key, val)) {
      dbgPrintf("[SER] Unknown key or invalid value: %s\n", key.c_str());
      return;
    }
    dbgPrintf("[SER] cfg set %s ok\n", key.c_str());
    return;
  }

  dbgPrintln(F("[SER] Unknown command. Type 'help'"));
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
  dbgPrintf("[WiFi] Connecting to %s", cfg.wifi_ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.wifi_ssid, cfg.wifi_password);
  for (int i = 0; i < 40; i++) {
    if (WiFi.status() == WL_CONNECTED) {
      dbgPrintf("\n[WiFi] IP: %s\n", WiFi.localIP().toString().c_str());
      return true;
    }
    delay(500); Serial.print('.'); yield();
  }
  dbgPrintln(F("\n[WiFi] Failed"));
  return false;
}

void startAP() {
  apMode = true;
  String ssid = String(F("WaterSensor-")) + String(ESP.getChipId(), HEX);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid.c_str(), "watersensor");
  dbgPrintf("[AP] SSID: %s  IP: %s\n", ssid.c_str(),
                WiFi.softAPIP().toString().c_str());
}

// ── NTP ──────────────────────────────────────────────────────────────────────
void setupNTP() {
  // Kyiv timezone (EET/EEST with DST)
  setenv("TZ", "EET-2EEST,M3.5.0/3,M10.5.0/4", 1);
  tzset();
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  // Wait for sync (max 5s)
  time_t t = 0;
  for (int i = 0; i < 10 && t < 1000000; i++) { delay(500); t = time(nullptr); }
  dbgPrintf("[NTP] Time: %lu (Kyiv TZ)\n", (unsigned long)t);
}

// ── OTA ──────────────────────────────────────────────────────────────────────
void setupArduinoOTA() {
  ArduinoOTA.setHostname(cfg.device_name);
  if (strlen(cfg.ota_pass)) ArduinoOTA.setPassword(cfg.ota_pass);
  ArduinoOTA.onStart([]{ dbgPrintln(F("[OTA] Start")); });
  ArduinoOTA.onEnd([]{ dbgPrintln(F("\n[OTA] End")); });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int t){
    dbgPrintf("[OTA] %u%%\r", p * 100 / t);
  });
  ArduinoOTA.onError([](ota_error_t e){
    dbgPrintf("[OTA] Error %u\n", e);
  });
  ArduinoOTA.begin();
}

// ── Measure callback ──────────────────────────────────────────────────────────
void doMeasureCallback() {
  doMeasure(cfg, sens);
  dbgPrintf("[Sensor] dist=%.1f cm  level=%.1f%%  vol=%.1f L  temp=%.1f°C\n",
                sens.distance_cm, sens.level_pct, sens.volume_liters, sens.temp_c);
  if (!bootPhase) tgCheckAlerts(cfg, sens);
}

// ── setup ────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(200);
  dbgPrintln(F("\n\n===== Water Level Sensor v1.0.0 ====="));
  dbgPrintln(F("[SER] Console ready. Type 'help'"));

  // LittleFS
  if (!LittleFS.begin()) {
    dbgPrintln(F("[FS] Format..."));
    LittleFS.format();
    LittleFS.begin();
  }

  // Config
  if (!loadConfig(cfg)) {
    dbgPrintln(F("[CFG] Using defaults"));
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
    tgSetMeasureCallback(doMeasureCallback);
    tgSetup(cfg);
  }

  // mDNS
  if (MDNS.begin(cfg.device_name)) {
    MDNS.addService("http", "tcp", 80);
    dbgPrintf("[mDNS] http://%s.local\n", cfg.device_name);
  }

  // Web server
  webSetup(webServer, updater, cfg, sens, doMeasureCallback);
  webServer.begin();
  dbgPrintln(F("[HTTP] Server started"));

  // First measurement
  doMeasureCallback();
  tMeasure = millis();

  // Store first point in history right away
  if (!apMode) {
    storageWrite(sens);
    tHourly = millis();
    tgBootMessage(cfg, sens);
  }
  bootPhase = false;
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
      dbgPrintln(F("[WiFi] Reconnecting..."));
      WiFi.reconnect();
      delay(5000);
    }
  }

}
