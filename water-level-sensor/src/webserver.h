#pragma once
#include <Arduino.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266WiFi.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "config.h"
#include "sensor.h"
#include "storage.h"
#include "mqtt_handler.h"
#include "telegram_handler.h"

// ------------------------------------------------------------------
// Serve static file from LittleFS with cache headers
// ------------------------------------------------------------------
static void serveFile(ESP8266WebServer &srv, const char *path, const char *mime) {
  File f = LittleFS.open(path, "r");
  if (!f) { srv.send(404, F("text/plain"), F("Not found")); return; }
  srv.sendHeader(F("Cache-Control"), F("max-age=86400"));
  srv.streamFile(f, mime);
  f.close();
}

// ------------------------------------------------------------------
// JSON helper
// ------------------------------------------------------------------
static void sendJson(ESP8266WebServer &srv, const String &json, int code = 200) {
  srv.send(code, F("application/json"), json);
}

// ------------------------------------------------------------------
// /api/wifi-scan
// ------------------------------------------------------------------
static String buildWifiScan() {
  int found = WiFi.scanNetworks();
  if (found < 0) found = 0;

  if (found > 20) found = 20; // keep response small for ESP8266 RAM

  DynamicJsonDocument doc(128 + found * 96);
  JsonArray nets = doc.createNestedArray("networks");

  for (int i = 0; i < found; i++) {
    String ssid = WiFi.SSID(i);
    if (!ssid.length()) continue; // skip hidden/empty SSID entries

    JsonObject n = nets.createNestedObject();
    n["ssid"] = ssid;
    n["rssi"] = WiFi.RSSI(i);
    n["enc"]  = (WiFi.encryptionType(i) != ENC_TYPE_NONE);
    n["ch"]   = WiFi.channel(i);
    yield();
  }

  WiFi.scanDelete();

  String out;
  serializeJson(doc, out);
  return out;
}

// ------------------------------------------------------------------
// /api/status
// ------------------------------------------------------------------
static String buildStatus(const Config &c, const SensorData &s) {
  StaticJsonDocument<512> doc;
  doc["level"]    = serialized(String(s.level_pct, 1));
  doc["distance"] = serialized(String(s.distance_cm, 1));
  doc["volume"]   = serialized(String(s.volume_liters, 1));
  doc["free"]     = serialized(String(s.free_liters, 1));
  doc["total"]    = serialized(String(s.total_liters, 1));
  if (!isnan(s.temp_c)) doc["temp"] = serialized(String(s.temp_c, 1));
  else                  doc["temp"] = nullptr;
  doc["valid"]    = s.valid;
  doc["ts"]       = s.timestamp;
  doc["ip"]       = WiFi.localIP().toString();
  doc["rssi"]     = WiFi.RSSI();
  doc["heap"]     = ESP.getFreeHeap();
  doc["diameter"] = c.barrel_diam_cm;
  doc["records"]  = storageCount();
  doc["wifi"]     = (WiFi.status() == WL_CONNECTED);
  doc["mqtt"]     = mqttConnected();
  doc["tg"]       = tgEnabled();
  doc["version"]  = FW_VERSION;
  String out; serializeJson(doc, out);
  return out;
}

// ------------------------------------------------------------------
// /api/history?h=N  (N hours, default 24, max 168)
// ------------------------------------------------------------------
static String buildHistory(int hours) {
  if (hours < 1)   hours = 24;
  if (hours > 168) hours = 168;

  HistRecord buf[MAX_REC];
  int cnt = storageRead(buf, MAX_REC);

  // Filter by time window
  uint32_t now = time(nullptr);
  uint32_t since = now - (uint32_t)hours * 3600UL;

  DynamicJsonDocument doc(cnt * 64 + 96);
  JsonArray labels = doc.createNestedArray("labels");
  JsonArray values = doc.createNestedArray("values");
  JsonArray vols   = doc.createNestedArray("volumes");
  JsonArray temps  = doc.createNestedArray("temps");

  // buf is newest-first; reverse to chronological
  for (int i = cnt - 1; i >= 0; i--) {
    if (buf[i].ts < since || buf[i].ts == 0) continue;
    struct tm *ti = localtime((time_t*)&buf[i].ts);
    char lbl[10];
    if (hours <= 24)
      strftime(lbl, sizeof(lbl), "%H:%M", ti);
    else
      strftime(lbl, sizeof(lbl), "%d.%m", ti);
    labels.add(lbl);
    values.add(serialized(String(buf[i].level, 1)));
    vols.add(serialized(String(buf[i].volume, 1)));
    if (isnan(buf[i].temp_c)) temps.add(nullptr);
    else                      temps.add(serialized(String(buf[i].temp_c, 1)));
  }

  String out; serializeJson(doc, out);
  return out;
}

// ------------------------------------------------------------------
// /api/export  — CSV download
// ------------------------------------------------------------------
static void handleExport(ESP8266WebServer &srv) {
  HistRecord buf[MAX_REC];
  int cnt = storageRead(buf, MAX_REC);

  srv.sendHeader(F("Content-Disposition"), F("attachment; filename=history.csv"));
  srv.setContentLength(CONTENT_LENGTH_UNKNOWN);
  srv.send(200, F("text/csv"), "");

  srv.sendContent(F("datetime,level_pct,volume_liters,temp_c\r\n"));
  for (int i = cnt - 1; i >= 0; i--) {
    if (buf[i].ts == 0) continue;
    struct tm *ti = localtime((time_t*)&buf[i].ts);
    char row[64];
    strftime(row, sizeof(row), "%Y-%m-%d %H:%M,", ti);
    srv.sendContent(row);
    srv.sendContent(String(buf[i].level, 1));
    srv.sendContent(",");
    srv.sendContent(String(buf[i].volume, 1));
    srv.sendContent(",");
    if (!isnan(buf[i].temp_c)) srv.sendContent(String(buf[i].temp_c, 1));
    srv.sendContent(F("\r\n"));
    yield();
  }
}

// ------------------------------------------------------------------
// Setup all routes
// ------------------------------------------------------------------
inline void webSetup(ESP8266WebServer &srv,
                     ESP8266HTTPUpdateServer &updater,
                     Config &cfg,
                     SensorData &sens,
                     std::function<void()> measureCallback)
{
  // Static files
  srv.on("/",          HTTP_GET,  [&]{ serveFile(srv, "/index.html", "text/html"); });
  srv.on("/index.html",HTTP_GET,  [&]{ serveFile(srv, "/index.html", "text/html"); });
  srv.on("/set.html",  HTTP_GET,  [&]{ serveFile(srv, "/set.html",   "text/html"); });
  srv.on("/s.css",     HTTP_GET,  [&]{ serveFile(srv, "/s.css",      "text/css");  });
  srv.on("/c.js",      HTTP_GET,  [&]{ serveFile(srv, "/c.js",       "application/javascript"); });

  // API - status
  srv.on("/api/status", HTTP_GET, [&]{
    srv.sendHeader(F("Access-Control-Allow-Origin"), "*");
    sendJson(srv, buildStatus(cfg, sens));
  });

  // API - history
  srv.on("/api/history", HTTP_GET, [&]{
    int h = srv.hasArg("h") ? srv.arg("h").toInt() : 24;
    sendJson(srv, buildHistory(h));
  });

  // API - measure now
  srv.on("/api/measure", HTTP_POST, [&]{
    measureCallback();
    sendJson(srv, buildStatus(cfg, sens));
  });

  // API - get config (mask passwords)
  srv.on("/api/config", HTTP_GET, [&]{
    StaticJsonDocument<768> doc;
    doc["ws"] = cfg.wifi_ssid;
    doc["wp"] = strlen(cfg.wifi_password) ? "••••••••" : "";
    doc["tp"] = cfg.trig_pin;
    doc["ep"] = cfg.echo_pin;
    doc["ed"] = cfg.empty_dist_cm;
    doc["fd"] = cfg.full_dist_cm;
    doc["bd"] = cfg.barrel_diam_cm;
    doc["as"] = cfg.avg_samples;
    doc["ms"] = cfg.measure_sec;
    doc["me"] = cfg.mqtt_en;
    doc["mh"] = cfg.mqtt_host;
    doc["mp"] = cfg.mqtt_port;
    doc["mu"] = cfg.mqtt_user;
    doc["mq"] = strlen(cfg.mqtt_pass) ? "••••••••" : "";
    doc["mt"] = cfg.mqtt_topic;
    doc["te"] = cfg.tg_en;
    doc["tt"] = strlen(cfg.tg_token) ? "••••••••" : "";
    doc["tc"] = cfg.tg_chat;
    doc["tl"] = cfg.tg_alert_low;
    doc["th"] = cfg.tg_alert_high;
    doc["td"] = cfg.tg_daily;
    doc["dp"] = cfg.ds18_pin;
    doc["de"] = cfg.ds18_en;
    doc["dn"] = cfg.device_name;
    doc["op"] = "";  // never expose OTA password
    String out; serializeJson(doc, out);
    sendJson(srv, out);
  });

  // API - scan WiFi networks
  srv.on("/api/wifi-scan", HTTP_GET, [&]{
    sendJson(srv, buildWifiScan());
  });

  // API - save config
  srv.on("/api/config", HTTP_POST, [&]{
    if (!srv.hasArg("plain")) { srv.send(400); return; }
    StaticJsonDocument<768> doc;
    if (deserializeJson(doc, srv.arg("plain"))) { srv.send(400, "text/plain", "Bad JSON"); return; }

    auto copyStr = [&](const char *key, char *dst, size_t len) {
      if (doc.containsKey(key) && doc[key].as<String>() != "••••••••")
        strlcpy(dst, doc[key] | "", len);
    };
    auto hasValue = [&](const char *key) -> bool {
      return doc.containsKey(key) && !doc[key].isNull();
    };

    copyStr("ws", cfg.wifi_ssid,    sizeof(cfg.wifi_ssid));
    copyStr("wp", cfg.wifi_password, sizeof(cfg.wifi_password));
    if (hasValue("tp")) cfg.trig_pin      = doc["tp"];
    if (hasValue("ep")) cfg.echo_pin      = doc["ep"];
    if (hasValue("ed")) cfg.empty_dist_cm = doc["ed"];
    if (hasValue("fd")) cfg.full_dist_cm  = doc["fd"];
    if (hasValue("bd")) cfg.barrel_diam_cm = doc["bd"];
    if (hasValue("as")) cfg.avg_samples   = doc["as"];
    if (hasValue("ms")) cfg.measure_sec   = doc["ms"];
    if (doc.containsKey("me")) cfg.mqtt_en        = doc["me"];
    copyStr("mh", cfg.mqtt_host,  sizeof(cfg.mqtt_host));
    if (hasValue("mp")) cfg.mqtt_port = doc["mp"];
    copyStr("mu", cfg.mqtt_user, sizeof(cfg.mqtt_user));
    copyStr("mq", cfg.mqtt_pass, sizeof(cfg.mqtt_pass));
    copyStr("mt", cfg.mqtt_topic, sizeof(cfg.mqtt_topic));
    if (doc.containsKey("te")) cfg.tg_en = doc["te"];
    copyStr("tt", cfg.tg_token, sizeof(cfg.tg_token));
    copyStr("tc", cfg.tg_chat,  sizeof(cfg.tg_chat));
    if (hasValue("tl")) cfg.tg_alert_low  = doc["tl"];
    if (hasValue("th")) cfg.tg_alert_high = doc["th"];
    if (doc.containsKey("td")) cfg.tg_daily  = doc["td"];
    if (hasValue("dp")) cfg.ds18_pin  = doc["dp"];
    if (doc.containsKey("de")) cfg.ds18_en   = doc["de"];
    copyStr("dn", cfg.device_name, sizeof(cfg.device_name));
    copyStr("op", cfg.ota_pass,    sizeof(cfg.ota_pass));

    bool ok = saveConfig(cfg);
    sendJson(srv, ok ? F("{\"ok\":true,\"reboot\":true}") : F("{\"ok\":false}"));
    if (ok) {
      delay(500);
      ESP.restart();
    }
  });

  // API - export CSV
  srv.on("/api/export", HTTP_GET, [&]{ handleExport(srv); });

  // API - clear history
  srv.on("/api/history", HTTP_DELETE, [&]{
    storageClear();
    sendJson(srv, F("{\"ok\":true}"));
  });

  // API - factory reset
  srv.on("/api/reset", HTTP_POST, [&]{
    LittleFS.remove(CONFIG_FILE);
    storageClear();
    sendJson(srv, F("{\"ok\":true}"));
    delay(500);
    ESP.restart();
  });

  // API - system info
  srv.on("/api/info", HTTP_GET, [&]{
    StaticJsonDocument<256> doc;
    doc["version"]   = FW_VERSION;
    doc["chip_id"]   = String(ESP.getChipId(), HEX);
    doc["flash"]     = ESP.getFlashChipSize();
    doc["sketch"]    = ESP.getSketchSize();
    doc["free_sketch"] = ESP.getFreeSketchSpace();
    doc["heap"]      = ESP.getFreeHeap();
    doc["uptime"]    = millis() / 1000;
    String out; serializeJson(doc, out);
    sendJson(srv, out);
  });

  // OTA web update
  updater.setup(&srv, "/update", "admin", cfg.ota_pass);

  // 404
  srv.onNotFound([&]{
    srv.send(404, F("text/plain"), F("Not found"));
  });
}
