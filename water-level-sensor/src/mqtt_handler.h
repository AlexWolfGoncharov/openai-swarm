#pragma once
#include <Arduino.h>
#include <PubSubClient.h>
#include <ESP8266WiFi.h>
#include <ArduinoJson.h>
#include "config.h"
#include "sensor.h"

static WiFiClient   _mqttWifiClient;
static PubSubClient _mqttClient(_mqttWifiClient);
static bool         _mqttEnabled = false;
static unsigned long _mqttLastAttempt = 0;
static bool         _mqttDiscoverySent = false;

// ── Availability topic ────────────────────────────────────────────────────────
static String _availTopic() {
  return String(F("watersensor/")) + String(ESP.getChipId(), HEX) + F("/status");
}

// ── Setup ─────────────────────────────────────────────────────────────────────
inline void mqttSetup(const Config &c) {
  if (!c.mqtt_en) return;
  _mqttEnabled = true;
  _mqttClient.setServer(c.mqtt_host, c.mqtt_port);
  _mqttClient.setBufferSize(512);   // needed for discovery payloads
}

// ── Connect (with LWT) ────────────────────────────────────────────────────────
static bool _mqttConnect(const Config &c) {
  if (_mqttClient.connected()) return true;
  if (millis() - _mqttLastAttempt < 15000UL) return false;
  _mqttLastAttempt = millis();

  String avail = _availTopic();
  const char *id = c.device_name[0] ? c.device_name : "watersensor";
  bool ok = strlen(c.mqtt_user)
    ? _mqttClient.connect(id, c.mqtt_user, c.mqtt_pass,
                          avail.c_str(), 0, true, "offline")
    : _mqttClient.connect(id, nullptr, nullptr,
                          avail.c_str(), 0, true, "offline");

  if (ok) {
    Serial.println(F("[MQTT] connected"));
    _mqttClient.publish(avail.c_str(), "online", true);
    _mqttDiscoverySent = false;   // re-send discovery after reconnect
  } else {
    Serial.printf("[MQTT] failed rc=%d\n", _mqttClient.state());
  }
  return ok;
}

// ── MQTT Auto-Discovery for Home Assistant ────────────────────────────────────
// Publishes to homeassistant/sensor/<unique_id>/config
// HA will automatically create entities without any YAML
// ─────────────────────────────────────────────────────────────────────────────
inline void mqttDiscovery(const Config &c) {
  if (!_mqttEnabled || _mqttDiscoverySent) return;
  if (!_mqttConnect(c)) return;

  String chipHex   = String(ESP.getChipId(), HEX);
  String devName   = c.device_name[0] ? String(c.device_name) : F("WaterSense");
  String avail     = _availTopic();

  // Shared device block (groups all sensors into one HA device)
  // Built separately as string to reuse across 4 payloads
  char devBlock[200];
  snprintf(devBlock, sizeof(devBlock),
    "{\"ids\":[\"ws_%s\"],\"name\":\"%s\",\"mdl\":\"WaterSense %s\",\"mf\":\"DIY ESP8266\",\"cu\":\"http://%s\"}",
    chipHex.c_str(), devName.c_str(), FW_VERSION, WiFi.localIP().toString().c_str());

  // Helper: publish one sensor discovery config
  auto pubDisc = [&](const char *uid_suffix,
                     const char *friendly_name,
                     const char *state_topic,
                     const char *unit,
                     const char *dev_class,   // "" = none
                     const char *icon) {
    char discTopic[90];
    snprintf(discTopic, sizeof(discTopic),
             "homeassistant/sensor/ws_%s_%s/config",
             chipHex.c_str(), uid_suffix);

    StaticJsonDocument<480> doc;
    doc[F("name")]         = friendly_name;
    doc[F("uniq_id")]      = String(F("ws_")) + chipHex + "_" + uid_suffix;
    doc[F("stat_t")]       = state_topic;
    doc[F("unit_of_meas")] = unit;
    doc[F("stat_cla")]     = F("measurement");
    doc[F("ic")]           = icon;
    doc[F("avty_t")]       = avail;
    doc[F("pl_avail")]     = F("online");
    doc[F("pl_not_avail")] = F("offline");
    doc[F("dev")]          = serialized(devBlock);
    if (strlen(dev_class)) doc[F("dev_cla")] = dev_class;

    char payload[480];
    size_t len = serializeJson(doc, payload, sizeof(payload));
    _mqttClient.publish(discTopic, payload, true);
    yield();
  };

  // Build state topics from config base topic
  String base = c.mqtt_topic;
  char tLevel[80], tVolume[80], tFree[80], tDist[80];
  snprintf(tLevel,  sizeof(tLevel),  "%s/level",    base.c_str());
  snprintf(tVolume, sizeof(tVolume), "%s/volume",   base.c_str());
  snprintf(tFree,   sizeof(tFree),   "%s/free",     base.c_str());
  snprintf(tDist,   sizeof(tDist),   "%s/distance", base.c_str());

  // Publish discovery for each entity
  pubDisc("level",    (devName + " Уровень").c_str(),     tLevel,  "%",  "",         "mdi:waves");
  pubDisc("volume",   (devName + " Объём").c_str(),       tVolume, "L",  "volume",   "mdi:barrel");
  pubDisc("free",     (devName + " Свободно").c_str(),    tFree,   "L",  "volume",   "mdi:barrel-outline");
  pubDisc("distance", (devName + " Расстояние").c_str(),  tDist,   "cm", "distance", "mdi:ruler");

  _mqttDiscoverySent = true;
  Serial.println(F("[MQTT] HA discovery published"));
}

// ── Loop ──────────────────────────────────────────────────────────────────────
inline void mqttLoop(const Config &c) {
  if (!_mqttEnabled) return;
  if (!_mqttConnect(c)) return;
  _mqttClient.loop();
}

// ── Publish sensor data ───────────────────────────────────────────────────────
inline void mqttPublish(const Config &c, const SensorData &s) {
  if (!_mqttEnabled || !s.valid) return;
  if (!_mqttConnect(c)) return;

  char topic[80], payload[32];

  snprintf(topic, sizeof(topic), "%s/level",    c.mqtt_topic);
  snprintf(payload, sizeof(payload), "%.1f", s.level_pct);
  _mqttClient.publish(topic, payload, true);

  snprintf(topic, sizeof(topic), "%s/distance", c.mqtt_topic);
  snprintf(payload, sizeof(payload), "%.1f", s.distance_cm);
  _mqttClient.publish(topic, payload, true);

  if (c.barrel_diam_cm > 0) {
    snprintf(topic, sizeof(topic), "%s/volume", c.mqtt_topic);
    snprintf(payload, sizeof(payload), "%.1f", s.volume_liters);
    _mqttClient.publish(topic, payload, true);

    snprintf(topic, sizeof(topic), "%s/free", c.mqtt_topic);
    snprintf(payload, sizeof(payload), "%.1f", s.free_liters);
    _mqttClient.publish(topic, payload, true);
  }

  // Full JSON message
  char json[200];
  snprintf(json, sizeof(json),
    "{\"level\":%.1f,\"dist\":%.1f,\"vol\":%.1f,\"free\":%.1f,\"ts\":%lu}",
    s.level_pct, s.distance_cm, s.volume_liters, s.free_liters, (unsigned long)s.timestamp);
  snprintf(topic, sizeof(topic), "%s/json", c.mqtt_topic);
  _mqttClient.publish(topic, json, true);
}

inline bool mqttConnected() {
  return _mqttEnabled && _mqttClient.connected();
}
