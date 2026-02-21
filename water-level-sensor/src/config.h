#pragma once
#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

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
  char     tg_token[128];
  char     tg_chat[32];
  float    tg_alert_low;     // alert when level falls below, %
  float    tg_alert_high;    // alert when level rises above, %
  bool     tg_daily;         // send daily summary at midnight

  // System
  char     device_name[32];
  char     ota_pass[32];
};

// ---------- defaults ----------
inline void configDefaults(Config &c) {
  memset(&c, 0, sizeof(c));
  strlcpy(c.wifi_ssid,    "",          sizeof(c.wifi_ssid));
  strlcpy(c.wifi_password,"",          sizeof(c.wifi_password));
  c.trig_pin       = 14;   // D5
  c.echo_pin       = 12;   // D6
  c.empty_dist_cm  = 100.0f;
  c.full_dist_cm   = 5.0f;
  c.barrel_diam_cm = 0.0f;
  c.avg_samples    = 5;
  c.measure_sec    = 60;
  c.mqtt_en        = false;
  strlcpy(c.mqtt_host,  "mqtt.local",  sizeof(c.mqtt_host));
  c.mqtt_port      = 1883;
  strlcpy(c.mqtt_topic, "watersensor", sizeof(c.mqtt_topic));
  c.tg_en          = false;
  c.tg_alert_low   = 20.0f;
  c.tg_alert_high  = 95.0f;
  c.tg_daily       = false;
  strlcpy(c.device_name, "watersensor", sizeof(c.device_name));
  strlcpy(c.ota_pass,    "ota1234",     sizeof(c.ota_pass));
}

// ---------- load ----------
inline bool loadConfig(Config &c) {
  configDefaults(c);
  File f = LittleFS.open(CONFIG_FILE, "r");
  if (!f) return false;

  StaticJsonDocument<1024> doc;
  if (deserializeJson(doc, f)) { f.close(); return false; }
  f.close();

  strlcpy(c.wifi_ssid,     doc["ws"]    | "",          sizeof(c.wifi_ssid));
  strlcpy(c.wifi_password, doc["wp"]    | "",          sizeof(c.wifi_password));
  c.trig_pin       = doc["tp"]  | 14;
  c.echo_pin       = doc["ep"]  | 12;
  c.empty_dist_cm  = doc["ed"]  | 100.0f;
  c.full_dist_cm   = doc["fd"]  | 5.0f;
  c.barrel_diam_cm = doc["bd"]  | 0.0f;
  c.avg_samples    = doc["as"]  | 5;
  c.measure_sec    = doc["ms"]  | 60;
  c.mqtt_en        = doc["me"]  | false;
  strlcpy(c.mqtt_host,  doc["mh"]  | "mqtt.local", sizeof(c.mqtt_host));
  c.mqtt_port      = doc["mp"]  | 1883;
  strlcpy(c.mqtt_user,  doc["mu"]  | "",           sizeof(c.mqtt_user));
  strlcpy(c.mqtt_pass,  doc["mq"]  | "",           sizeof(c.mqtt_pass));
  strlcpy(c.mqtt_topic, doc["mt"]  | "watersensor",sizeof(c.mqtt_topic));
  c.tg_en          = doc["te"]  | false;
  strlcpy(c.tg_token,   doc["tt"]  | "",           sizeof(c.tg_token));
  strlcpy(c.tg_chat,    doc["tc"]  | "",           sizeof(c.tg_chat));
  c.tg_alert_low   = doc["tl"]  | 20.0f;
  c.tg_alert_high  = doc["th"]  | 95.0f;
  c.tg_daily       = doc["td"]  | false;
  strlcpy(c.device_name, doc["dn"] | "watersensor", sizeof(c.device_name));
  strlcpy(c.ota_pass,    doc["op"] | "ota1234",     sizeof(c.ota_pass));
  return true;
}

// ---------- save ----------
inline bool saveConfig(const Config &c) {
  StaticJsonDocument<1024> doc;
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
  doc["tt"] = c.tg_token;
  doc["tc"] = c.tg_chat;
  doc["tl"] = c.tg_alert_low;
  doc["th"] = c.tg_alert_high;
  doc["td"] = c.tg_daily;
  doc["dn"] = c.device_name;
  doc["op"] = c.ota_pass;

  File f = LittleFS.open(CONFIG_FILE, "w");
  if (!f) return false;
  serializeJson(doc, f);
  f.close();
  return true;
}
