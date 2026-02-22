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
// Median measurement: collect samples, sort, return middle value.
// Single-echo outliers (spikes) are discarded automatically.
// ------------------------------------------------------------------
#define SENSOR_MAX_SAMPLES 30

inline float measureDistance(const Config &c) {
  float buf[SENSOR_MAX_SAMPLES];
  int   ok = 0;
  int   n  = min((int)c.avg_samples, SENSOR_MAX_SAMPLES);

  for (int i = 0; i < n; i++) {
    float d = _hcsr04_once(c.trig_pin, c.echo_pin);
    if (d > 0 && d < 500) buf[ok++] = d;
    delay(50);
    yield();
  }
  if (ok == 0) return -1.0f;

  // Insertion sort (fast for ≤30 elements)
  for (int i = 1; i < ok; i++) {
    float key = buf[i];
    int   j   = i - 1;
    while (j >= 0 && buf[j] > key) { buf[j + 1] = buf[j]; j--; }
    buf[j + 1] = key;
  }

  // Return median
  if (ok % 2 == 1) return buf[ok / 2];
  return (buf[ok / 2 - 1] + buf[ok / 2]) / 2.0f;
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
  dbgPrintf("[DS18B20] init on GPIO%u  devices: %u\n",
                c.ds18_pin, _ds18_dt->getDeviceCount());
}

// ------------------------------------------------------------------
// Full measurement cycle with inter-cycle EMA smoothing
// ------------------------------------------------------------------
inline void doMeasure(const Config &c, SensorData &s) {
  static float _ema_dist = -1.0f;

  // Kick off DS18B20 conversion before HC-SR04 (runs concurrently)
  if (_ds18_dt) _ds18_dt->requestTemperatures();

  float dist = measureDistance(c);   // ~avg_samples × 50 ms

  if (dist < 0) {
    // No valid reading — hold last EMA, mark invalid
    computeLevel(c, _ema_dist > 0 ? _ema_dist : 0, s);
    s.valid = false;
  } else {
    // First valid reading initialises the filter; subsequent readings blend in
    if (_ema_dist < 0) {
      _ema_dist = dist;
    } else {
      float alpha = constrain(c.ema_alpha, 0.01f, 1.0f);
      _ema_dist = alpha * dist + (1.0f - alpha) * _ema_dist;
    }
    computeLevel(c, _ema_dist, s);
  }

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
