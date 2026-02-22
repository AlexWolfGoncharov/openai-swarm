#pragma once
#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "config.h"

struct SensorData {
  float    distance_cm;    // raw measured distance, cm
  float    level_pct;      // 0..100 %
  float    volume_liters;  // current volume, L  (0 if diameter unknown)
  float    free_liters;    // free space volume, L
  float    total_liters;   // total barrel volume, L
  float    temp_c;         // DS18B20 temperature, °C  (NAN if unavailable)
  uint32_t timestamp;      // Unix time of last reading
  bool     valid;          // measurement OK
};

// DS18B20 driver state (allocated on first initTempSensor call)
static OneWire*           _ds18_ow = nullptr;
static DallasTemperature* _ds18_dt = nullptr;

// ------------------------------------------------------------------
// Single HC-SR04 pulse measurement, returns distance in cm or -1 on timeout
// ------------------------------------------------------------------
static float _hcsr04_once(uint8_t trig, uint8_t echo) {
  digitalWrite(trig, LOW);
  delayMicroseconds(2);
  digitalWrite(trig, HIGH);
  delayMicroseconds(10);
  digitalWrite(trig, LOW);

  unsigned long dur = pulseIn(echo, HIGH, 30000UL); // 30 ms timeout ≈ 5 m
  if (dur == 0) return -1.0f;
  return dur * 0.01715f;  // cm = µs × (34300 cm/s / 2 / 1e6)
}

// ------------------------------------------------------------------
// Averaged measurement
// ------------------------------------------------------------------
inline float measureDistance(const Config &c) {
  float sum = 0;
  int   ok  = 0;
  for (int i = 0; i < c.avg_samples; i++) {
    float d = _hcsr04_once(c.trig_pin, c.echo_pin);
    if (d > 0 && d < 500) { sum += d; ok++; }
    delay(50);
    yield();
  }
  return ok ? sum / ok : -1.0f;
}

// ------------------------------------------------------------------
// Compute level% and volumes from raw distance
// ------------------------------------------------------------------
inline void computeLevel(const Config &c, float dist_cm, SensorData &s) {
  s.distance_cm = dist_cm;
  s.valid       = (dist_cm > 0);

  // level%: 0 when empty (dist == empty_dist), 100 when full (dist == full_dist)
  float range = c.empty_dist_cm - c.full_dist_cm;
  if (!s.valid || range <= 0) {
    s.level_pct     = 0;
    s.volume_liters = 0;
    s.free_liters   = 0;
    s.total_liters  = 0;
    return;
  }
  float pct = (c.empty_dist_cm - dist_cm) / range * 100.0f;
  s.level_pct = constrain(pct, 0.0f, 100.0f);

  if (c.barrel_diam_cm > 0) {
    float r  = c.barrel_diam_cm / 2.0f;
    float h  = c.empty_dist_cm - c.full_dist_cm;          // barrel height, cm
    s.total_liters  = PI * r * r * h / 1000.0f;            // cm³ → L
    s.volume_liters = s.total_liters * s.level_pct / 100.0f;
    s.free_liters   = s.total_liters - s.volume_liters;
  } else {
    s.total_liters = s.volume_liters = s.free_liters = 0;
  }
}

// ------------------------------------------------------------------
// DS18B20 initialisation (call once after config is loaded)
// ------------------------------------------------------------------
inline void initTempSensor(const Config &c) {
  if (!c.ds18_en) return;
  _ds18_ow = new OneWire(c.ds18_pin);
  _ds18_dt = new DallasTemperature(_ds18_ow);
  _ds18_dt->begin();
  _ds18_dt->setResolution(10);           // 10-bit ≈ 187 ms conversion
  _ds18_dt->setWaitForConversion(false); // async — we poll below
  Serial.printf("[DS18B20] init on GPIO%u  devices: %u\n",
                c.ds18_pin, _ds18_dt->getDeviceCount());
}

// ------------------------------------------------------------------
// Full measurement cycle
// ------------------------------------------------------------------
inline void doMeasure(const Config &c, SensorData &s) {
  // Kick off DS18B20 conversion before HC-SR04 (runs concurrently)
  if (_ds18_dt) _ds18_dt->requestTemperatures();

  float dist = measureDistance(c);   // ~50–250 ms depending on avg_samples
  computeLevel(c, dist, s);

  // Wait for DS18B20 conversion to finish (max 200 ms at 10-bit)
  if (_ds18_dt) {
    unsigned long tw = millis();
    while (!_ds18_dt->isConversionComplete() && millis() - tw < 250UL) {
      delay(5); yield();
    }
    float t = _ds18_dt->getTempCByIndex(0);
    s.temp_c = (t == DEVICE_DISCONNECTED_C) ? NAN : t;
  } else {
    s.temp_c = NAN;
  }

  s.timestamp = time(nullptr);
}

// ------------------------------------------------------------------
// Pin setup
// ------------------------------------------------------------------
inline void initSensor(const Config &c) {
  pinMode(c.trig_pin, OUTPUT);
  pinMode(c.echo_pin, INPUT);
  digitalWrite(c.trig_pin, LOW);
}
