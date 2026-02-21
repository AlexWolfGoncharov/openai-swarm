#pragma once
#include <Arduino.h>
#include <PubSubClient.h>
#include <ESP8266WiFi.h>
#include "config.h"
#include "sensor.h"

static WiFiClient   _mqttWifiClient;
static PubSubClient _mqttClient(_mqttWifiClient);
static bool         _mqttEnabled = false;
static unsigned long _mqttLastAttempt = 0;

inline void mqttSetup(const Config &c) {
  if (!c.mqtt_en) return;
  _mqttEnabled = true;
  _mqttClient.setServer(c.mqtt_host, c.mqtt_port);
}

static bool _mqttConnect(const Config &c) {
  if (_mqttClient.connected()) return true;
  if (millis() - _mqttLastAttempt < 15000UL) return false;
  _mqttLastAttempt = millis();

  const char *id = c.device_name[0] ? c.device_name : "watersensor";
  bool ok = strlen(c.mqtt_user)
    ? _mqttClient.connect(id, c.mqtt_user, c.mqtt_pass)
    : _mqttClient.connect(id);

  if (ok) Serial.println(F("[MQTT] connected"));
  else    Serial.printf("[MQTT] failed rc=%d\n", _mqttClient.state());
  return ok;
}

inline void mqttLoop(const Config &c) {
  if (!_mqttEnabled) return;
  if (!_mqttConnect(c)) return;
  _mqttClient.loop();
}

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
    snprintf(topic, sizeof(topic), "%s/volume",   c.mqtt_topic);
    snprintf(payload, sizeof(payload), "%.1f", s.volume_liters);
    _mqttClient.publish(topic, payload, true);

    snprintf(topic, sizeof(topic), "%s/free",     c.mqtt_topic);
    snprintf(payload, sizeof(payload), "%.1f", s.free_liters);
    _mqttClient.publish(topic, payload, true);
  }

  // Home-Assistant / generic JSON status
  char json[200];
  snprintf(json, sizeof(json),
    "{\"level\":%.1f,\"dist\":%.1f,\"vol\":%.1f,\"free\":%.1f}",
    s.level_pct, s.distance_cm, s.volume_liters, s.free_liters);
  snprintf(topic, sizeof(topic), "%s/json", c.mqtt_topic);
  _mqttClient.publish(topic, json, true);
}

inline bool mqttConnected() {
  return _mqttEnabled && _mqttClient.connected();
}
