#pragma once
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <ArduinoJson.h>
#include "config.h"
#include "debug_log.h"

// ------------------------------------------------------------------
// Parse semantic version string "X.Y.Z" into a comparable integer.
// Returns 0 on parse failure.
// ------------------------------------------------------------------
static uint32_t _parseVersion(const char *v) {
  unsigned int ma = 0, mi = 0, pa = 0;
  sscanf(v, "%u.%u.%u", &ma, &mi, &pa);
  return ma * 1000000UL + mi * 1000UL + pa;
}

// ------------------------------------------------------------------
// checkFirmwareUpdate()
//
// Downloads cfg.ota_version_url (must return JSON like):
//   { "version": "1.2.3", "url": "https://.../firmware.bin" }
//
// Compares with the running FW_VERSION. If remote is strictly newer,
// calls ESPhttpUpdate which flashes and reboots the device.
//
// Uses BearSSL::WiFiClientSecure with setInsecure() — appropriate
// for a home device downloading from a trusted self-controlled repo.
// Certificate pinning can be added later if needed.
//
// Returns true if update was triggered (device will reboot shortly).
// Returns false on no-update or any error.
// ------------------------------------------------------------------
inline bool checkFirmwareUpdate(const Config &c) {
  if (!c.ota_auto_en) return false;
  if (!strlen(c.ota_version_url)) {
    dbgPrintln(F("[OTA-auto] No version URL configured"));
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    dbgPrintln(F("[OTA-auto] WiFi not connected, skipping"));
    return false;
  }

  dbgPrintf("[OTA-auto] Checking %s (running %s)\n", c.ota_version_url, FW_VERSION);

  BearSSL::WiFiClientSecure client;
  client.setInsecure();          // no cert verification — trusted own server
  client.setTimeout(15000);

  HTTPClient http;
  if (!http.begin(client, c.ota_version_url)) {
    dbgPrintln(F("[OTA-auto] http.begin failed"));
    return false;
  }
  http.setTimeout(12000);
  http.addHeader(F("User-Agent"), F("ESP8266-WaterSensor-OTA/1.0"));

  int code = http.GET();
  if (code != 200) {
    dbgPrintf("[OTA-auto] HTTP %d, skipping\n", code);
    http.end();
    return false;
  }

  // Parse JSON from stream to avoid allocating the whole body as String
  StaticJsonDocument<256> doc;
  auto err = deserializeJson(doc, http.getStream());
  http.end();

  if (err) {
    dbgPrintf("[OTA-auto] JSON error: %s\n", err.c_str());
    return false;
  }

  const char *remoteVer = doc["version"] | "";
  const char *binUrl    = doc["url"]     | "";

  if (!strlen(remoteVer) || !strlen(binUrl)) {
    dbgPrintln(F("[OTA-auto] Missing 'version' or 'url' in response"));
    return false;
  }

  uint32_t remoteNum  = _parseVersion(remoteVer);
  uint32_t currentNum = _parseVersion(FW_VERSION);

  if (remoteNum <= currentNum) {
    dbgPrintf("[OTA-auto] Already up to date (%s >= %s)\n", FW_VERSION, remoteVer);
    return false;
  }

  dbgPrintf("[OTA-auto] Updating %s -> %s from %s\n", FW_VERSION, remoteVer, binUrl);

  BearSSL::WiFiClientSecure updateClient;
  updateClient.setInsecure();

  ESPhttpUpdate.setLedPin(LED_BUILTIN, LOW);
  ESPhttpUpdate.rebootOnUpdate(true);

  t_httpUpdate_return ret = ESPhttpUpdate.update(updateClient, binUrl);
  switch (ret) {
    case HTTP_UPDATE_FAILED:
      dbgPrintf("[OTA-auto] FAILED: (%d) %s\n",
                ESPhttpUpdate.getLastError(),
                ESPhttpUpdate.getLastErrorString().c_str());
      break;
    case HTTP_UPDATE_NO_UPDATES:
      dbgPrintln(F("[OTA-auto] Server says no update"));
      break;
    case HTTP_UPDATE_OK:
      dbgPrintln(F("[OTA-auto] OK — rebooting"));
      return true;  // device will reboot via rebootOnUpdate(true)
  }
  return false;
}
