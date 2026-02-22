#pragma once
#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "debug_log.h"

#define CONFIG_FILE "/config.json"
#define FW_VERSION  "1.0.0"

struct Config {
  // WiFi
  char wifi_ssid[64];
  char wifi_password[64];

  // Sensor pins & calibration
  uint8_t  trig_pin;         // default D5 (GPIO14)
  uint8_t  echo_pin;         // default D6 (GPIO12)
  float    empty_dist_cm;    // distance when barrel is EMPTY (sensor→bottom)
  float    full_dist_cm;     // distance when barrel is FULL  (sensor→water)
  float    barrel_diam_cm;   // inner diameter, cm (0 = unknown)
  uint8_t  avg_samples;      // readings to average (1..10)
  uint16_t measure_sec;      // measurement interval, seconds

  // MQTT
  bool     mqtt_en;
  char     mqtt_host[64];
  uint16_t mqtt_port;
  char     mqtt_user[32];
  char     mqtt_pass[32];
  char     mqtt_topic[64];   // base topic; /level /volume /status published

  // Telegram
  bool     tg_en;
  bool     tg_cmd_en;       // poll bot commands (/status, /measure)
  bool     tg_alert_low_en;   // low-level threshold alert
  bool     tg_alert_high_en;  // high-level threshold alert
  bool     tg_boot_msg_en;  // startup status message
  char     tg_token[128];
  char     tg_chat[32];
  float    tg_alert_low;     // alert when level falls below, %
  float    tg_alert_high;    // alert when level rises above, %
  bool     tg_daily;         // send daily summary at midnight

  // DS18B20 temperature sensor
  uint8_t  ds18_pin;         // data pin (default GPIO2 = D4)
  bool     ds18_en;          // enable temperature sensor

  // System
  char     device_name[32];
  char     ota_pass[32];
};

// ---------- defaults ----------
inline void configDefaults(Config &c) {
  memset(&c, 0, sizeof(c));
  // Defaults below are prefilled for the current installation.
  strlcpy(c.wifi_ssid,    "Katya_5G:)", sizeof(c.wifi_ssid));
  strlcpy(c.wifi_password,"30101986",   sizeof(c.wifi_password));
  c.trig_pin       = 14;   // D5
  c.echo_pin       = 12;   // D6
  c.empty_dist_cm  = 110.0f;
  c.full_dist_cm   = 25.0f;
  c.barrel_diam_cm = 51.0f;
  c.avg_samples    = 10;
  c.measure_sec    = 60;
  c.mqtt_en        = true;
  strlcpy(c.mqtt_host,  "192.168.4.107", sizeof(c.mqtt_host));
  c.mqtt_port      = 1883;
  strlcpy(c.mqtt_topic, "watersensor", sizeof(c.mqtt_topic));
  c.tg_en          = true;
  c.tg_cmd_en      = true;
  c.tg_alert_low_en  = false;
  c.tg_alert_high_en = false;
  c.tg_boot_msg_en = true;
  strlcpy(c.tg_chat, "125791364", sizeof(c.tg_chat)); // token is intentionally not embedded
  c.tg_alert_low   = 20.0f;
  c.tg_alert_high  = 95.0f;
  c.tg_daily       = true;
  c.ds18_pin       = 2;     // D4 = GPIO2
  c.ds18_en        = true;
  strlcpy(c.device_name, "watersensor", sizeof(c.device_name));
  strlcpy(c.ota_pass,    "ota1234",     sizeof(c.ota_pass));
}

inline void logConfigSummary(const char *tag, const Config &c) {
  dbgPrintf(
    "[CFG] %s | wifi_ssid='%s' trig=%u echo=%u ds18_en=%u ds18_pin=%u "
    "empty=%.1f full=%.1f diam=%.1f avg=%u sec=%u mqtt=%u tg=%u tx=%u tal=%u tah=%u tb=%u\n",
    tag ? tag : "state",
    c.wifi_ssid,
    c.trig_pin, c.echo_pin,
    c.ds18_en ? 1 : 0, c.ds18_pin,
    c.empty_dist_cm, c.full_dist_cm, c.barrel_diam_cm,
    c.avg_samples, c.measure_sec,
    c.mqtt_en ? 1 : 0, c.tg_en ? 1 : 0,
    c.tg_cmd_en ? 1 : 0,
    c.tg_alert_low_en ? 1 : 0,
    c.tg_alert_high_en ? 1 : 0,
    c.tg_boot_msg_en ? 1 : 0
  );
}

inline bool sanitizeConfig(Config &c) {
  bool changed = false;

  auto fixU8 = [&](uint8_t &v, uint8_t def, const char *name) {
    if (v == 0 || v > 16) {
      dbgPrintf("[CFG] Sanitize %s: %u -> %u\n", name, v, def);
      v = def;
      changed = true;
    }
  };

  fixU8(c.trig_pin, 14, "trig_pin");
  fixU8(c.echo_pin, 12, "echo_pin");
  fixU8(c.ds18_pin, 2,  "ds18_pin");

  if (c.avg_samples < 1 || c.avg_samples > 10) {
    uint8_t old = c.avg_samples;
    c.avg_samples = 10;
    dbgPrintf("[CFG] Sanitize avg_samples: %u -> %u\n", old, c.avg_samples);
    changed = true;
  }

  if (c.measure_sec < 10 || c.measure_sec > 3600) {
    uint16_t old = c.measure_sec;
    c.measure_sec = 60;
    dbgPrintf("[CFG] Sanitize measure_sec: %u -> %u\n", old, c.measure_sec);
    changed = true;
  }

  if (c.mqtt_port == 0) {
    dbgPrintf("[CFG] Sanitize mqtt_port: %u -> %u\n", c.mqtt_port, 1883);
    c.mqtt_port = 1883;
    changed = true;
  }

  if (c.barrel_diam_cm < 0 || c.barrel_diam_cm > 10000) {
    float old = c.barrel_diam_cm;
    c.barrel_diam_cm = 51.0f;
    dbgPrintf("[CFG] Sanitize barrel_diam_cm: %.2f -> %.2f\n", old, c.barrel_diam_cm);
    changed = true;
  }

  if (c.empty_dist_cm <= 0 || c.full_dist_cm <= 0 || c.full_dist_cm >= c.empty_dist_cm) {
    dbgPrintf("[CFG] Sanitize distances: empty=%.2f full=%.2f -> empty=110.00 full=25.00\n",
                  c.empty_dist_cm, c.full_dist_cm);
    c.empty_dist_cm = 110.0f;
    c.full_dist_cm  = 25.0f;
    changed = true;
  }

  if (c.tg_alert_low <= 0 || c.tg_alert_low >= 100) {
    float old = c.tg_alert_low;
    c.tg_alert_low = 20.0f;
    dbgPrintf("[CFG] Sanitize tg_alert_low: %.2f -> %.2f\n", old, c.tg_alert_low);
    changed = true;
  }
  if (c.tg_alert_high <= 0 || c.tg_alert_high > 100 || c.tg_alert_high <= c.tg_alert_low) {
    float old = c.tg_alert_high;
    c.tg_alert_high = 95.0f;
    dbgPrintf("[CFG] Sanitize tg_alert_high: %.2f -> %.2f\n", old, c.tg_alert_high);
    changed = true;
  }

  if (!strlen(c.device_name)) {
    strlcpy(c.device_name, "watersensor", sizeof(c.device_name));
    dbgPrintln(F("[CFG] Sanitize device_name: empty -> watersensor"));
    changed = true;
  }
  if (!strlen(c.mqtt_topic)) {
    strlcpy(c.mqtt_topic, "watersensor", sizeof(c.mqtt_topic));
    dbgPrintln(F("[CFG] Sanitize mqtt_topic: empty -> watersensor"));
    changed = true;
  }

  if (changed) logConfigSummary("sanitized", c);
  return changed;
}

// ---------- load ----------
inline bool loadConfig(Config &c) {
  configDefaults(c);
  File f = LittleFS.open(CONFIG_FILE, "r");
  if (!f) {
    dbgPrintln(F("[CFG] /config.json not found, using defaults"));
    logConfigSummary("defaults", c);
    return false;
  }

  StaticJsonDocument<1536> doc;
  if (deserializeJson(doc, f)) {
    f.close();
    dbgPrintln(F("[CFG] Bad config.json, using defaults"));
    logConfigSummary("defaults", c);
    return false;
  }
  f.close();

  strlcpy(c.wifi_ssid,     doc["ws"]    | "",          sizeof(c.wifi_ssid));
  strlcpy(c.wifi_password, doc["wp"]    | "",          sizeof(c.wifi_password));
  c.trig_pin       = doc["tp"]  | 14;
  c.echo_pin       = doc["ep"]  | 12;
  c.empty_dist_cm  = doc["ed"]  | 110.0f;
  c.full_dist_cm   = doc["fd"]  | 25.0f;
  c.barrel_diam_cm = doc["bd"]  | 51.0f;
  c.avg_samples    = doc["as"]  | 10;
  c.measure_sec    = doc["ms"]  | 60;
  c.mqtt_en        = doc["me"]  | true;
  strlcpy(c.mqtt_host,  doc["mh"]  | "192.168.4.107", sizeof(c.mqtt_host));
  c.mqtt_port      = doc["mp"]  | 1883;
  strlcpy(c.mqtt_user,  doc["mu"]  | "",           sizeof(c.mqtt_user));
  strlcpy(c.mqtt_pass,  doc["mq"]  | "",           sizeof(c.mqtt_pass));
  strlcpy(c.mqtt_topic, doc["mt"]  | "watersensor",sizeof(c.mqtt_topic));
  c.tg_en          = doc["te"]  | true;
  c.tg_cmd_en      = doc["tx"]  | true;
  bool legacy_ta = doc["ta"] | false;
  c.tg_alert_low_en  = doc.containsKey("tal") ? (bool)doc["tal"] : legacy_ta;
  c.tg_alert_high_en = doc.containsKey("tah") ? (bool)doc["tah"] : legacy_ta;
  c.tg_boot_msg_en = doc["tb"]  | true;
  strlcpy(c.tg_token,   doc["tt"]  | "",           sizeof(c.tg_token));
  strlcpy(c.tg_chat,    doc["tc"]  | "",           sizeof(c.tg_chat));
  c.tg_alert_low   = doc["tl"]  | 20.0f;
  c.tg_alert_high  = doc["th"]  | 95.0f;
  c.tg_daily       = doc["td"]  | true;
  c.ds18_pin       = doc["dp"]  | 2;
  c.ds18_en        = doc["de"]  | true;
  strlcpy(c.device_name, doc["dn"] | "watersensor", sizeof(c.device_name));
  strlcpy(c.ota_pass,    doc["op"] | "ota1234",     sizeof(c.ota_pass));
  sanitizeConfig(c);
  logConfigSummary("loaded", c);
  return true;
}

// ---------- save ----------
inline bool saveConfig(Config &c) {
  sanitizeConfig(c);
  StaticJsonDocument<1536> doc;
  doc["ws"] = c.wifi_ssid;
  doc["wp"] = c.wifi_password;
  doc["tp"] = c.trig_pin;
  doc["ep"] = c.echo_pin;
  doc["ed"] = c.empty_dist_cm;
  doc["fd"] = c.full_dist_cm;
  doc["bd"] = c.barrel_diam_cm;
  doc["as"] = c.avg_samples;
  doc["ms"] = c.measure_sec;
  doc["me"] = c.mqtt_en;
  doc["mh"] = c.mqtt_host;
  doc["mp"] = c.mqtt_port;
  doc["mu"] = c.mqtt_user;
  doc["mq"] = c.mqtt_pass;
  doc["mt"] = c.mqtt_topic;
  doc["te"] = c.tg_en;
  doc["tx"] = c.tg_cmd_en;
  doc["tal"] = c.tg_alert_low_en;
  doc["tah"] = c.tg_alert_high_en;
  doc["tb"] = c.tg_boot_msg_en;
  doc["tt"] = c.tg_token;
  doc["tc"] = c.tg_chat;
  doc["tl"] = c.tg_alert_low;
  doc["th"] = c.tg_alert_high;
  doc["td"] = c.tg_daily;
  doc["dp"] = c.ds18_pin;
  doc["de"] = c.ds18_en;
  doc["dn"] = c.device_name;
  doc["op"] = c.ota_pass;

  File f = LittleFS.open(CONFIG_FILE, "w");
  if (!f) {
    dbgPrintln(F("[CFG] Failed to open /config.json for write"));
    return false;
  }
  serializeJson(doc, f);
  f.close();
  logConfigSummary("saved", c);
  return true;
}
