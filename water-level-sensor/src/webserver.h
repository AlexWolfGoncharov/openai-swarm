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
  int scanned = found;

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
  dbgPrintf("[WEB] GET /api/wifi-scan -> %d scanned, %u bytes\n", scanned, out.length());
  return out;
}

// ------------------------------------------------------------------
// /api/status
// ------------------------------------------------------------------
static String buildStatus(const Config &c, const SensorData &s) {
  StaticJsonDocument<512> doc;
  auto r1 = [](float v) { return roundf(v * 10.0f) / 10.0f; };
  doc["level"]    = r1(s.level_pct);
  doc["distance"] = r1(s.distance_cm);
  doc["volume"]   = r1(s.volume_liters);
  doc["free"]     = r1(s.free_liters);
  doc["total"]    = r1(s.total_liters);
  if (!isnan(s.temp_c)) doc["temp"] = r1(s.temp_c);
  else                  doc["temp"] = nullptr;
  doc["valid"]    = s.valid;
  doc["ts"]       = s.timestamp;
  doc["ip"]       = WiFi.localIP().toString();
  doc["rssi"]     = WiFi.RSSI();
  doc["heap"]     = ESP.getFreeHeap();
  doc["diameter"] = c.barrel_diam_cm;
  doc["records"]  = storageCount();
  doc["records_max"] = MAX_REC;
  doc["wifi"]     = (WiFi.status() == WL_CONNECTED);
  doc["mqtt"]     = mqttConnected();
  doc["tg"]       = tgEnabled();
  doc["version"]  = FW_VERSION;
  String out; serializeJson(doc, out);
  return out;
}

// ------------------------------------------------------------------
// /api/history?h=N  (N hours, default 24, max 168 for UI chart)
// ------------------------------------------------------------------
static String buildHistory(int hours) {
  const int HISTORY_UI_MAX_HOURS = 168;
  if (hours < 1)   hours = 24;
  if (hours > HISTORY_UI_MAX_HOURS) hours = HISTORY_UI_MAX_HOURS;

  static HistRecord buf[168]; // keep RAM predictable for chart responses
  int cnt = storageRead(buf, HISTORY_UI_MAX_HOURS);

  // Filter by time window
  uint32_t now = time(nullptr);
  uint32_t since = now - (uint32_t)hours * 3600UL;

  DynamicJsonDocument doc(cnt * 80 + 128);
  JsonArray labels = doc.createNestedArray("labels");
  JsonArray values = doc.createNestedArray("values");
  JsonArray vols   = doc.createNestedArray("volumes");
  JsonArray temps  = doc.createNestedArray("temps");

  // buf is newest-first; reverse to chronological
  for (int i = cnt - 1; i >= 0; i--) {
    yield(); // keep /api/history responsive on ESP8266
    if (buf[i].ts < since || buf[i].ts == 0) continue;
    time_t ts = (time_t)buf[i].ts;
    struct tm *ti = localtime(&ts);
    if (!ti) continue;
    char lbl[10];
    if (hours <= 24)
      strftime(lbl, sizeof(lbl), "%H:%M", ti);
    else
      strftime(lbl, sizeof(lbl), "%d.%m", ti);
    labels.add(lbl);
    values.add(roundf(buf[i].level * 10.0f) / 10.0f);
    vols.add(roundf(buf[i].volume * 10.0f) / 10.0f);
    if (isnan(buf[i].temp_c)) temps.add(nullptr);
    else                      temps.add(roundf(buf[i].temp_c * 10.0f) / 10.0f);
  }

  String out; serializeJson(doc, out);
  return out;
}

// ------------------------------------------------------------------
// /api/export  — CSV download
// ------------------------------------------------------------------
static void handleExport(ESP8266WebServer &srv) {
  srv.sendHeader(F("Content-Disposition"), F("attachment; filename=history.csv"));
  srv.setContentLength(CONTENT_LENGTH_UNKNOWN);
  srv.send(200, F("text/csv"), "");

  srv.sendContent(F("datetime,level_pct,volume_liters,temp_c\r\n"));
  File f = LittleFS.open(HIST_FILE, "r");
  if (!f) return;

  HistHeader hdr;
  if (f.size() != (size_t)(sizeof(HistHeader) + MAX_REC * sizeof(HistRecord)) ||
      f.read((uint8_t*)&hdr, sizeof(hdr)) != sizeof(hdr) ||
      hdr.head >= MAX_REC || hdr.count > MAX_REC) {
    f.close();
    return;
  }

  HistRecord rec;
  int start = ((int)hdr.head - (int)hdr.count + MAX_REC) % MAX_REC; // oldest
  for (uint16_t i = 0; i < hdr.count; i++) {
    int idx = (start + i) % MAX_REC;
    f.seek(sizeof(HistHeader) + idx * sizeof(HistRecord));
    if (f.read((uint8_t*)&rec, sizeof(rec)) != sizeof(rec)) break;
    if (rec.ts == 0) continue;
    time_t ts = (time_t)rec.ts;
    struct tm *ti = localtime(&ts);
    if (!ti) continue;
    char row[64];
    strftime(row, sizeof(row), "%Y-%m-%d %H:%M,", ti);
    srv.sendContent(row);
    srv.sendContent(String(rec.level, 1));
    srv.sendContent(",");
    srv.sendContent(String(rec.volume, 1));
    srv.sendContent(",");
    if (!isnan(rec.temp_c)) srv.sendContent(String(rec.temp_c, 1));
    srv.sendContent(F("\r\n"));
    yield();
  }
  f.close();
}

// ------------------------------------------------------------------
// /api/logs  — recent mirrored serial logs (ring buffer)
// ------------------------------------------------------------------
static String buildDebugLogs() {
  DynamicJsonDocument doc(6144);
  doc["uptime"] = millis() / 1000;
  JsonArray lines = doc.createNestedArray("lines");
  uint8_t cnt = dbgLogCount();
  for (uint8_t i = 0; i < cnt; i++) {
    lines.add(dbgLogLineAt(i));
    yield();
  }
  String out;
  serializeJson(doc, out);
  return out;
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

  // API - debug logs
  srv.on("/api/logs", HTTP_GET, [&]{
    sendJson(srv, buildDebugLogs());
  });

  // API - measure now
  srv.on("/api/measure", HTTP_POST, [&]{
    dbgPrintln(F("[WEB] POST /api/measure"));
    measureCallback();
    sendJson(srv, buildStatus(cfg, sens));
  });

  // API - get config (mask passwords)
  srv.on("/api/config", HTTP_GET, [&]{
    dbgPrintln(F("[WEB] GET /api/config"));
    StaticJsonDocument<1024> doc;
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
    doc["tx"] = cfg.tg_cmd_en;
    doc["tal"] = cfg.tg_alert_low_en;
    doc["tah"] = cfg.tg_alert_high_en;
    doc["tb"] = cfg.tg_boot_msg_en;
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
    dbgPrintln(F("[WEB] POST /api/config"));
    if (!srv.hasArg("plain")) {
      dbgPrintln(F("[WEB] POST /api/config -> 400 (no body)"));
      srv.send(400);
      return;
    }
    StaticJsonDocument<1024> doc;
    if (deserializeJson(doc, srv.arg("plain"))) {
      dbgPrintln(F("[WEB] POST /api/config -> 400 (bad JSON)"));
      srv.send(400, "text/plain", "Bad JSON");
      return;
    }

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
    if (doc.containsKey("tx")) cfg.tg_cmd_en = doc["tx"];
    if (doc.containsKey("ta")) { // legacy UI compatibility
      bool v = doc["ta"];
      cfg.tg_alert_low_en = v;
      cfg.tg_alert_high_en = v;
    }
    if (doc.containsKey("tal")) cfg.tg_alert_low_en = doc["tal"];
    if (doc.containsKey("tah")) cfg.tg_alert_high_en = doc["tah"];
    if (doc.containsKey("tb")) cfg.tg_boot_msg_en = doc["tb"];
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
    dbgPrintf("[WEB] POST /api/config -> %s (reboot=%u)\n", ok ? "ok" : "fail", ok ? 1 : 0);
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
    dbgPrintln(F("[WEB] DELETE /api/history"));
    storageClear();
    sendJson(srv, F("{\"ok\":true}"));
  });

  // API - factory reset
  srv.on("/api/reset", HTTP_POST, [&]{
    dbgPrintln(F("[WEB] POST /api/reset"));
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
