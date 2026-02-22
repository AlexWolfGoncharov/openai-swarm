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
  // Avoid stale UI after OTA/uploadfs: don't cache HTML, cache assets aggressively.
  if (strcmp(mime, "text/html") == 0) {
    srv.sendHeader(F("Cache-Control"), F("no-store, max-age=0"));
    srv.sendHeader(F("Pragma"), F("no-cache"));
  } else {
    srv.sendHeader(F("Cache-Control"), F("max-age=86400"));
  }
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
struct TrendStats {
  bool     ok = false;
  float    used24_l = NAN;
  float    used7d_l = NAN;
  float    rate24_lpd = NAN;
  float    rate7d_lpd = NAN;
  uint32_t span24_s = 0;
  uint32_t span7d_s = 0;
  float    days_left = NAN;
  uint32_t eta_empty_ts = 0;
};

static TrendStats _trendCache;
static uint32_t   _trendCacheForTs = 0;

static TrendStats computeTrendStats(const SensorData &s) {
  if (_trendCacheForTs == s.timestamp && s.timestamp != 0) return _trendCache;

  TrendStats st;
  _trendCacheForTs = s.timestamp;

  if (s.timestamp == 0 || s.total_liters <= 0 || s.volume_liters < 0) {
    _trendCache = st;
    return st;
  }

  static HistRecord hbuf[168];
  static HistRecord rbuf[MAX_RECENT_REC];
  int hcnt = storageRead(hbuf, 168);
  int rcnt = storageReadRecent(rbuf, MAX_RECENT_REC);

  const uint32_t now = s.timestamp;
  const uint32_t since24 = (now > 24UL * 3600UL) ? (now - 24UL * 3600UL) : 0;
  const uint32_t since7d = (now > 168UL * 3600UL) ? (now - 168UL * 3600UL) : 0;
  const uint32_t recentSince = (now > 3600UL) ? (now - 3600UL) : 0;

  bool havePrev = false;
  HistRecord prev = {0,0,0,NAN};
  bool have24Point = false, have7Point = false;
  uint32_t first24 = 0, last24 = 0, first7 = 0, last7 = 0;
  float used24 = 0.0f, used7 = 0.0f;

  auto feed = [&](const HistRecord &rec) {
    if (rec.ts == 0 || rec.ts > now) return;
    if (rec.ts < since7d) return;
    if (!(rec.volume >= 0.0f)) return;

    if (rec.ts >= since24) {
      if (!have24Point) first24 = rec.ts;
      last24 = rec.ts;
      have24Point = true;
    }
    if (rec.ts >= since7d) {
      if (!have7Point) first7 = rec.ts;
      last7 = rec.ts;
      have7Point = true;
    }

    if (havePrev && rec.ts > prev.ts && prev.volume >= 0.0f) {
      uint32_t dt = rec.ts - prev.ts;
      // Ignore large gaps and impossible rates to reduce noise after reboots/manual changes.
      if (dt >= 30 && dt <= 6UL * 3600UL) {
        float dv = rec.volume - prev.volume; // + = refill, - = consumption/leak
        if (dv < -0.3f) {
          if (prev.ts >= since24 && rec.ts >= since24) used24 += -dv;
          if (prev.ts >= since7d && rec.ts >= since7d) used7  += -dv;
        }
      }
    }

    prev = rec;
    havePrev = true;
  };

  // Older hourly points (exclude last hour - replaced by minute cache)
  for (int i = hcnt - 1; i >= 0; i--) {
    yield();
    if (hbuf[i].ts >= recentSince) continue;
    feed(hbuf[i]);
  }
  // Recent minute points
  for (int i = rcnt - 1; i >= 0; i--) {
    yield();
    feed(rbuf[i]);
  }

  st.ok = true;
  if (have24Point && last24 > first24) {
    st.used24_l = roundf(used24 * 10.0f) / 10.0f;
    st.span24_s = last24 - first24;
    st.rate24_lpd = roundf((used24 * 86400.0f / st.span24_s) * 10.0f) / 10.0f;
  }
  if (have7Point && last7 > first7) {
    st.used7d_l = roundf(used7 * 10.0f) / 10.0f;
    st.span7d_s = last7 - first7;
    st.rate7d_lpd = roundf((used7 * 86400.0f / st.span7d_s) * 10.0f) / 10.0f;
  }

  float useRate = NAN;
  if (!isnan(st.rate24_lpd) && st.span24_s >= 6UL * 3600UL && st.rate24_lpd > 0.2f) useRate = st.rate24_lpd;
  else if (!isnan(st.rate7d_lpd) && st.span7d_s >= 24UL * 3600UL && st.rate7d_lpd > 0.2f) useRate = st.rate7d_lpd;
  if (!isnan(useRate) && s.volume_liters > 0.0f) {
    st.days_left = roundf((s.volume_liters / useRate) * 10.0f) / 10.0f;
    st.eta_empty_ts = now + (uint32_t)((s.volume_liters / useRate) * 86400.0f);
  }

  _trendCache = st;
  return st;
}

static String buildRecentEvents() {
  static HistRecord rbuf[MAX_RECENT_REC];
  int rcnt = storageReadRecent(rbuf, MAX_RECENT_REC);

  struct EventRec {
    uint32_t ts;
    char type[8]; // fill|draw|leak
    float delta_l;
    float rate_lph;
  };
  EventRec evs[8];
  int evCount = 0;

  bool havePrev = false;
  HistRecord prev = {0,0,0,NAN};
  for (int i = rcnt - 1; i >= 0; i--) { // chronological
    const HistRecord &rec = rbuf[i];
    if (rec.ts == 0 || !(rec.volume >= 0.0f)) continue;
    if (!havePrev) { prev = rec; havePrev = true; continue; }
    if (rec.ts <= prev.ts) { prev = rec; continue; }

    uint32_t dt = rec.ts - prev.ts;
    if (dt < 30 || dt > 20UL * 60UL) { prev = rec; continue; }

    float dv = rec.volume - prev.volume;
    float rate = dv * 3600.0f / (float)dt;
    const char *type = nullptr;

    // Thresholds tuned for noisy barrel readings: detect meaningful changes only.
    if (dv <= -4.0f && rate <= -18.0f) type = "leak";
    else if (dv >= 6.0f)               type = "fill";
    else if (dv <= -6.0f)              type = "draw";

    if (type) {
      bool merged = false;
      if (evCount > 0) {
        EventRec &last = evs[evCount - 1];
        if (strcmp(last.type, type) == 0 && rec.ts - last.ts <= 15UL * 60UL) {
          last.ts = rec.ts;
          last.delta_l += dv;
          if (fabsf(rate) > fabsf(last.rate_lph)) last.rate_lph = rate;
          merged = true;
        }
      }
      if (!merged) {
        EventRec e = { rec.ts, "", dv, rate };
        strlcpy(e.type, type, sizeof(e.type));
        if (evCount < (int)(sizeof(evs)/sizeof(evs[0]))) evs[evCount++] = e;
        else {
          memmove(&evs[0], &evs[1], sizeof(evs) - sizeof(evs[0]));
          evs[evCount - 1] = e;
        }
      }
    }
    prev = rec;
  }

  DynamicJsonDocument doc(2048);
  JsonArray arr = doc.createNestedArray("events");
  for (int i = evCount - 1; i >= 0; i--) { // newest first
    JsonObject o = arr.createNestedObject();
    o["ts"] = evs[i].ts;
    o["type"] = evs[i].type;
    o["delta_l"] = roundf(evs[i].delta_l * 10.0f) / 10.0f;
    o["rate_lph"] = roundf(evs[i].rate_lph * 10.0f) / 10.0f;
  }
  doc["window_min"] = 60;
  String out; serializeJson(doc, out);
  return out;
}

static String buildStatus(const Config &c, const SensorData &s) {
  DynamicJsonDocument doc(1536);
  auto r1 = [](float v) { return roundf(v * 10.0f) / 10.0f; };
  TrendStats tr = computeTrendStats(s);
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
  doc["records_recent"] = storageCountRecent();
  doc["records_max"] = MAX_REC;
  doc["wifi"]     = (WiFi.status() == WL_CONNECTED);
  doc["mqtt"]     = mqttConnected();
  doc["tg"]       = tgEnabled();
  doc["version"]  = FW_VERSION;
  if (!isnan(tr.used24_l)) doc["used24"] = tr.used24_l; else doc["used24"] = nullptr;
  if (!isnan(tr.used7d_l)) doc["used7d"] = tr.used7d_l; else doc["used7d"] = nullptr;
  if (!isnan(tr.rate24_lpd)) doc["rate24"] = tr.rate24_lpd; else doc["rate24"] = nullptr;
  if (!isnan(tr.rate7d_lpd)) doc["rate7d"] = tr.rate7d_lpd; else doc["rate7d"] = nullptr;
  if (!isnan(tr.days_left)) doc["daysleft"] = tr.days_left; else doc["daysleft"] = nullptr;
  if (tr.eta_empty_ts) doc["eta_empty_ts"] = tr.eta_empty_ts; else doc["eta_empty_ts"] = nullptr;
  doc["span24"] = tr.span24_s;
  doc["span7d"] = tr.span7d_s;
  String out; serializeJson(doc, out);
  return out;
}

// ------------------------------------------------------------------
// /api/history?h=N  (N hours, default 24, supports up to MAX_REC with downsampling)
// ------------------------------------------------------------------
static String buildHistory(int hours) {
  const int HISTORY_UI_MAX_HOURS = MAX_REC;
  if (hours < 1)   hours = 24;
  if (hours > HISTORY_UI_MAX_HOURS) hours = HISTORY_UI_MAX_HOURS;

  static HistRecord rbuf[MAX_RECENT_REC];      // recent minute snapshots
  int rcnt = storageReadRecent(rbuf, MAX_RECENT_REC);

  // Filter by time window
  uint32_t now = time(nullptr);
  uint32_t since = now - (uint32_t)hours * 3600UL;
  uint32_t recentSince = (now > 3600UL) ? (now - 3600UL) : 0;

  // Downsample older hourly points for long ranges to keep RAM/JSON response small.
  int olderEligible = 0;
  {
    File hf = LittleFS.open(HIST_FILE, "r");
    HistHeader hdr;
    if (hf &&
        hf.size() == (size_t)(sizeof(HistHeader) + (size_t)MAX_REC * sizeof(HistRecord)) &&
        hf.read((uint8_t*)&hdr, sizeof(hdr)) == sizeof(hdr) &&
        hdr.head < MAX_REC && hdr.count <= MAX_REC) {
      HistRecord rec;
      int start = ((int)hdr.head - (int)hdr.count + MAX_REC) % MAX_REC; // oldest
      for (uint16_t i = 0; i < hdr.count; i++) {
        int idx = (start + i) % MAX_REC;
        hf.seek(sizeof(HistHeader) + idx * sizeof(HistRecord));
        if (hf.read((uint8_t*)&rec, sizeof(rec)) != sizeof(rec)) break;
        if (rec.ts == 0 || rec.ts < since) continue;
        if (rec.ts >= recentSince) continue;
        olderEligible++;
        yield();
      }
    }
    if (hf) hf.close();
  }
  int recentEligible = 0;
  for (int i = rcnt - 1; i >= 0; i--) {
    if (rbuf[i].ts == 0 || rbuf[i].ts < since) continue;
    recentEligible++;
  }
  int olderTarget = olderEligible; // keep all for short ranges
  if (hours > 168) {
    olderTarget = (hours > 720) ? 80 : 110; // + recent(<=60) => ~140..170 points total
    if (olderTarget < 1) olderTarget = 1;
  }
  int olderStride = (olderEligible > olderTarget) ? ((olderEligible + olderTarget - 1) / olderTarget) : 1;
  int outCount = recentEligible;
  if (olderEligible > 0) {
    outCount += (olderStride <= 1) ? olderEligible : ((olderEligible + olderStride - 1) / olderStride);
    // Ensure the most recent hourly point before recent window is not lost.
    if (olderStride > 1) outCount += 1;
  }
  if (outCount < 1) outCount = 1;

  DynamicJsonDocument doc(outCount * 96 + 256);
  JsonArray labels = doc.createNestedArray("labels");
  JsonArray values = doc.createNestedArray("values");
  JsonArray vols   = doc.createNestedArray("volumes");
  JsonArray temps  = doc.createNestedArray("temps");
  doc["hours"] = hours;
  doc["downsample"] = (olderStride > 1);

  auto addRec = [&](const HistRecord &rec) {
    if (rec.ts < since || rec.ts == 0) return;
    time_t ts = (time_t)rec.ts;
    struct tm *ti = localtime(&ts);
    if (!ti) return;
    char lbl[10];
    if (hours <= 24) {
      strftime(lbl, sizeof(lbl), "%H:%M", ti);
    } else if (rec.ts >= recentSince) {
      strftime(lbl, sizeof(lbl), "%H:%M", ti); // finer labels inside recent minute window
    } else {
      strftime(lbl, sizeof(lbl), "%d.%m", ti);
    }
    labels.add(lbl);
    values.add(roundf(rec.level * 10.0f) / 10.0f);
    vols.add(roundf(rec.volume * 10.0f) / 10.0f);
    if (isnan(rec.temp_c)) temps.add(nullptr);
    else                   temps.add(roundf(rec.temp_c * 10.0f) / 10.0f);
  };

  // Hourly part (older than the recent minute window), chronological
  int olderSeen = 0;
  {
    File hf = LittleFS.open(HIST_FILE, "r");
    HistHeader hdr;
    if (hf &&
        hf.size() == (size_t)(sizeof(HistHeader) + (size_t)MAX_REC * sizeof(HistRecord)) &&
        hf.read((uint8_t*)&hdr, sizeof(hdr)) == sizeof(hdr) &&
        hdr.head < MAX_REC && hdr.count <= MAX_REC) {
      HistRecord rec;
      int start = ((int)hdr.head - (int)hdr.count + MAX_REC) % MAX_REC; // oldest
      for (uint16_t i = 0; i < hdr.count; i++) {
        int idx = (start + i) % MAX_REC;
        hf.seek(sizeof(HistHeader) + idx * sizeof(HistRecord));
        if (hf.read((uint8_t*)&rec, sizeof(rec)) != sizeof(rec)) break;
        yield(); // keep /api/history responsive on ESP8266
        if (rec.ts >= recentSince) continue; // replaced by minute-resolution data
        if (rec.ts < since || rec.ts == 0) continue;
        bool keep = true;
        if (olderStride > 1) {
          keep = (olderSeen % olderStride) == 0 || olderSeen == olderEligible - 1;
        }
        if (keep) addRec(rec);
        olderSeen++;
      }
    }
    if (hf) hf.close();
  }

  // Recent part (last 60 minutes), chronological
  for (int i = rcnt - 1; i >= 0; i--) {
    yield();
    addRec(rbuf[i]);
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
// /api/config.raw  — exact config.json backup/restore (includes secrets)
// ------------------------------------------------------------------
static void handleConfigRawDownload(ESP8266WebServer &srv) {
  if (!LittleFS.exists(CONFIG_FILE)) {
    srv.send(404, F("text/plain"), F("No config"));
    return;
  }
  File f = LittleFS.open(CONFIG_FILE, "r");
  if (!f) { srv.send(500, F("text/plain"), F("Open failed")); return; }
  size_t sz = f.size();
  srv.sendHeader(F("Content-Disposition"), F("attachment; filename=config.json"));
  srv.sendHeader(F("Cache-Control"), F("no-store"));
  srv.setContentLength(sz);
  srv.streamFile(f, F("application/json"));
  f.close();
  dbgPrintf("[WEB] GET /api/config.raw -> %u bytes\n", (unsigned)sz);
}

static void handleConfigRawRestore(ESP8266WebServer &srv) {
  dbgPrintln(F("[WEB] POST /api/config.raw"));
  if (!srv.hasArg("plain")) {
    sendJson(srv, F("{\"ok\":false,\"err\":\"no_body\"}"), 400);
    return;
  }
  String body = srv.arg("plain");
  if (body.length() < 2 || body.length() > 4096) {
    sendJson(srv, F("{\"ok\":false,\"err\":\"bad_size\"}"), 400);
    return;
  }

  // Validate JSON shape first (full semantic validation will happen on load/sanitize).
  DynamicJsonDocument doc(3072);
  auto err = deserializeJson(doc, body);
  if (err) {
    dbgPrintf("[WEB] POST /api/config.raw -> bad JSON: %s\n", err.c_str());
    sendJson(srv, F("{\"ok\":false,\"err\":\"bad_json\"}"), 400);
    return;
  }

  const char *TMP_CONFIG_FILE = "/config.json.tmp";
  LittleFS.remove(TMP_CONFIG_FILE);
  File f = LittleFS.open(TMP_CONFIG_FILE, "w");
  if (!f) {
    sendJson(srv, F("{\"ok\":false,\"err\":\"open_tmp\"}"), 500);
    return;
  }
  size_t wr = f.print(body);
  f.flush();
  f.close();
  if (wr != (size_t)body.length()) {
    LittleFS.remove(TMP_CONFIG_FILE);
    sendJson(srv, F("{\"ok\":false,\"err\":\"write_failed\"}"), 500);
    return;
  }

  // Validate temp file parse before replacing live config.
  {
    File vf = LittleFS.open(TMP_CONFIG_FILE, "r");
    if (!vf) {
      LittleFS.remove(TMP_CONFIG_FILE);
      sendJson(srv, F("{\"ok\":false,\"err\":\"reopen_tmp\"}"), 500);
      return;
    }
    DynamicJsonDocument vdoc(3072);
    auto verr = deserializeJson(vdoc, vf);
    vf.close();
    if (verr) {
      LittleFS.remove(TMP_CONFIG_FILE);
      sendJson(srv, F("{\"ok\":false,\"err\":\"invalid_tmp\"}"), 400);
      return;
    }
  }

  LittleFS.remove(CONFIG_FILE);
  if (!LittleFS.rename(TMP_CONFIG_FILE, CONFIG_FILE)) {
    LittleFS.remove(TMP_CONFIG_FILE);
    sendJson(srv, F("{\"ok\":false,\"err\":\"replace_failed\"}"), 500);
    return;
  }

  dbgPrintln(F("[WEB] POST /api/config.raw -> ok"));
  sendJson(srv, F("{\"ok\":true,\"reboot\":false}"));
}

// ------------------------------------------------------------------
// /api/history*.bin  — raw ring backup/restore (exact binary files)
// Use multipart upload (curl -F file=@...)
// ------------------------------------------------------------------
struct HistUploadState {
  File file;
  bool ok = false;
  size_t written = 0;
  size_t maxBytes = 0;
  bool overflow = false;
};

static HistUploadState gHistUploadHourly;
static HistUploadState gHistUploadRecent;

static void handleHistoryBinDownload(ESP8266WebServer &srv,
                                     const char *path,
                                     uint16_t maxRec,
                                     const char *downloadName) {
  if (!storageValidateRingFile(path, maxRec)) {
    _storageInitRing(path, maxRec);
  }
  File f = LittleFS.open(path, "r");
  if (!f) { srv.send(500, F("text/plain"), F("Open failed")); return; }
  size_t sz = f.size();
  srv.sendHeader(F("Content-Disposition"), String(F("attachment; filename=")) + downloadName);
  srv.sendHeader(F("Cache-Control"), F("no-store"));
  srv.setContentLength(sz);
  srv.streamFile(f, F("application/octet-stream"));
  f.close();
  dbgPrintf("[WEB] GET %s -> %s (%u bytes)\n", path, downloadName, (unsigned)sz);
}

static void handleHistoryBinUploadChunk(ESP8266WebServer &srv,
                                        HistUploadState &st,
                                        const char *tmpPath,
                                        size_t expectedBytes) {
  HTTPUpload &upload = srv.upload();
  switch (upload.status) {
    case UPLOAD_FILE_START:
      dbgPrintf("[WEB] POST history upload start: %s -> %s\n", upload.filename.c_str(), tmpPath);
      if (st.file) st.file.close();
      LittleFS.remove(tmpPath);
      st.file = LittleFS.open(tmpPath, "w");
      st.ok = (bool)st.file;
      st.written = 0;
      st.maxBytes = expectedBytes;
      st.overflow = false;
      break;
    case UPLOAD_FILE_WRITE:
      if (!st.file || !st.ok) break;
      if (st.written + upload.currentSize > st.maxBytes) {
        st.ok = false;
        st.overflow = true;
        dbgPrintf("[WEB] POST history upload overflow: %u > %u\n",
                  (unsigned)(st.written + upload.currentSize), (unsigned)st.maxBytes);
        break;
      }
      if (st.file.write(upload.buf, upload.currentSize) != upload.currentSize) st.ok = false;
      else st.written += upload.currentSize;
      yield();
      break;
    case UPLOAD_FILE_END:
      if (st.file) st.file.close();
      if (st.written != st.maxBytes) {
        st.ok = false;
        dbgPrintf("[WEB] POST history upload wrong size: %u != %u\n",
                  (unsigned)st.written, (unsigned)st.maxBytes);
      }
      dbgPrintf("[WEB] POST history upload end: %u bytes (ok=%u)\n", (unsigned)st.written, st.ok ? 1 : 0);
      break;
    case UPLOAD_FILE_ABORTED:
      if (st.file) st.file.close();
      LittleFS.remove(tmpPath);
      st.ok = false;
      st.written = 0;
      st.maxBytes = 0;
      st.overflow = false;
      dbgPrintln(F("[WEB] POST history upload aborted"));
      break;
    default:
      break;
  }
}

static void handleHistoryBinUploadFinalize(ESP8266WebServer &srv,
                                           HistUploadState &st,
                                           const char *tmpPath,
                                           const char *dstPath,
                                           uint16_t maxRec,
                                           const char *kind) {
  if (st.file) st.file.close();
  if (!st.ok || !LittleFS.exists(tmpPath)) {
    LittleFS.remove(tmpPath);
    st.ok = false;
    st.written = 0;
    st.maxBytes = 0;
    bool ovf = st.overflow;
    st.overflow = false;
    dbgPrintf("[WEB] POST %s restore -> 400 (%s)\n", kind, ovf ? "too_large" : "upload failed");
    sendJson(srv, ovf ? F("{\"ok\":false,\"err\":\"file_too_large\"}") : F("{\"ok\":false,\"err\":\"upload\"}"), 400);
    return;
  }
  if (!storageReplaceRingFile(tmpPath, dstPath, maxRec)) {
    LittleFS.remove(tmpPath);
    st.ok = false;
    st.written = 0;
    st.maxBytes = 0;
    st.overflow = false;
    dbgPrintf("[WEB] POST %s restore -> 400 (invalid format)\n", kind);
    sendJson(srv, F("{\"ok\":false,\"err\":\"invalid_history_file\"}"), 400);
    return;
  }
  HistHeader hdr = {0,0};
  storageValidateRingFile(dstPath, maxRec, &hdr);
  _trendCacheForTs = 0;
  StaticJsonDocument<128> doc;
  doc["ok"] = true;
  doc["kind"] = kind;
  doc["count"] = hdr.count;
  doc["max"] = maxRec;
  String out; serializeJson(doc, out);
  st.ok = false;
  st.written = 0;
  st.maxBytes = 0;
  st.overflow = false;
  dbgPrintf("[WEB] POST %s restore -> ok (count=%u)\n", kind, hdr.count);
  sendJson(srv, out);
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
                     std::function<void()> measureCallback,
                     std::function<void()> queueMeasureCallback)
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

  // API - derived recent events (fill/drain/leak detection from minute history)
  srv.on("/api/events", HTTP_GET, [&]{
    sendJson(srv, buildRecentEvents());
  });

  // API - debug logs
  srv.on("/api/logs", HTTP_GET, [&]{
    sendJson(srv, buildDebugLogs());
  });

  // API - measure now
  srv.on("/api/measure", HTTP_POST, [&]{
    dbgPrintln(F("[WEB] POST /api/measure"));
    queueMeasureCallback();
    srv.sendHeader(F("Connection"), F("close"));
    sendJson(srv, buildStatus(cfg, sens));
  });

  // API - get config (mask passwords)
  srv.on("/api/config", HTTP_GET, [&]{
    dbgPrintln(F("[WEB] GET /api/config"));
    DynamicJsonDocument doc(2048);
    doc["ws"] = cfg.wifi_ssid;
    doc["wp"] = strlen(cfg.wifi_password) ? "••••••••" : "";
    doc["tp"] = cfg.trig_pin;
    doc["ep"] = cfg.echo_pin;
    doc["ed"] = cfg.empty_dist_cm;
    doc["fd"] = cfg.full_dist_cm;
    doc["bd"] = cfg.barrel_diam_cm;
    doc["as"] = cfg.avg_samples;
    doc["ea"] = cfg.ema_alpha;
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
    doc["ua"] = cfg.ota_auto_en;
    doc["uu"] = cfg.ota_version_url;
    doc["ui"] = cfg.ota_check_interval_h;
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
    DynamicJsonDocument doc(3072);
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
    if (hasValue("as")) cfg.avg_samples = doc["as"];
    if (hasValue("ea")) cfg.ema_alpha   = doc["ea"];
    if (hasValue("ms")) cfg.measure_sec = doc["ms"];
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
    if (doc.containsKey("de")) cfg.ds18_en = doc["de"];
    if (doc.containsKey("ua")) cfg.ota_auto_en = doc["ua"];
    copyStr("uu", cfg.ota_version_url, sizeof(cfg.ota_version_url));
    if (hasValue("ui")) cfg.ota_check_interval_h = doc["ui"];
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

  // API - exact config backup/restore (includes secrets)
  srv.on("/api/config.raw", HTTP_GET, [&]{ handleConfigRawDownload(srv); });
  srv.on("/api/config.raw", HTTP_POST, [&]{ handleConfigRawRestore(srv); });

  // API - exact binary backup/restore for history rings
  srv.on("/api/history.bin", HTTP_GET, [&]{
    handleHistoryBinDownload(srv, HIST_FILE, MAX_REC, "history-hourly.bin");
  });
  srv.on("/api/history_recent.bin", HTTP_GET, [&]{
    handleHistoryBinDownload(srv, HIST_RECENT_FILE, MAX_RECENT_REC, "history-recent.bin");
  });
  srv.on("/api/history.bin", HTTP_POST,
    [&]{
      handleHistoryBinUploadFinalize(srv, gHistUploadHourly, "/hist_hourly.upload.tmp", HIST_FILE, MAX_REC, "hourly");
    },
    [&]{
      handleHistoryBinUploadChunk(srv, gHistUploadHourly, "/hist_hourly.upload.tmp",
                                  sizeof(HistHeader) + (size_t)MAX_REC * sizeof(HistRecord));
    }
  );
  srv.on("/api/history_recent.bin", HTTP_POST,
    [&]{
      handleHistoryBinUploadFinalize(srv, gHistUploadRecent, "/hist_recent.upload.tmp", HIST_RECENT_FILE, MAX_RECENT_REC, "recent");
    },
    [&]{
      handleHistoryBinUploadChunk(srv, gHistUploadRecent, "/hist_recent.upload.tmp",
                                  sizeof(HistHeader) + (size_t)MAX_RECENT_REC * sizeof(HistRecord));
    }
  );

  // API - clear history
  srv.on("/api/history", HTTP_DELETE, [&]{
    dbgPrintln(F("[WEB] DELETE /api/history"));
    storageClear();
    _trendCacheForTs = 0;
    sendJson(srv, F("{\"ok\":true}"));
  });

  // API - factory reset
  srv.on("/api/reset", HTTP_POST, [&]{
    dbgPrintln(F("[WEB] POST /api/reset"));
    LittleFS.remove(CONFIG_FILE);
    storageClear();
    _trendCacheForTs = 0;
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
