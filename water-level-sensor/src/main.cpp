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
  Serial.printf("[Sensor] dist=%.1f cm  level=%.1f%%  vol=%.1f L\n",
                sens.distance_cm, sens.level_pct, sens.volume_liters);
  tgCheckAlerts(cfg, sens);
}

// ── setup ────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println(F("\n\n===== Water Level Sensor v1.0.0 ====="));

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
  webServer.handleClient();
  MDNS.update();
  yield();

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

  yield();
}
