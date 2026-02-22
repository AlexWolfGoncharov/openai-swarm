#pragma once
#include <Arduino.h>
#include <LittleFS.h>
#include "sensor.h"

// Circular buffers stored in LittleFS:
// - Hourly history (long-term)
// - Recent minute snapshots (last 60 min)
// Format:  header(4 bytes)  + MAX_REC * Record(16 bytes)
//   header: uint16 head, uint16 count
//   Record: uint32 ts + float level_pct + float volume_liters + float temp_c

#define HIST_FILE        "/hist.bin"
#define HIST_RECENT_FILE "/hist_recent.bin"
#define MAX_REC          2160  // 90 days × 24 h (hourly snapshots)
#define MAX_RECENT_REC   60    // last 60 minutes (1 point / minute)

struct HistRecord {
  uint32_t ts;
  float    level;    // %
  float    volume;   // L (0 if unknown)
  float    temp_c;   // °C (NAN if unavailable)
};

struct HistHeader {
  uint16_t head;   // next write index
  uint16_t count;  // total stored (0..MAX_REC)
};

inline bool _storageInitRing(const char *path, uint16_t maxRec) {
  // Recreate history if file format/size changed (e.g. firmware upgrade).
  if (LittleFS.exists(path)) {
    File f = LittleFS.open(path, "r");
    bool valid = false;
    if (f) {
      HistHeader hdr;
      if (f.size() == (size_t)(sizeof(HistHeader) + (size_t)maxRec * sizeof(HistRecord)) &&
          f.read((uint8_t*)&hdr, sizeof(hdr)) == sizeof(hdr) &&
          hdr.head < maxRec && hdr.count <= maxRec) {
        valid = true;
      }
      f.close();
    }
    if (valid) return true;
    LittleFS.remove(path);
  }
  // Create blank file
  File f = LittleFS.open(path, "w");
  if (!f) return false;
  HistHeader hdr = {0, 0};
  f.write((uint8_t*)&hdr, sizeof(hdr));
  // Pre-allocate space
  HistRecord blank = {0, 0, 0, NAN};
  for (uint16_t i = 0; i < maxRec; i++) f.write((uint8_t*)&blank, sizeof(blank));
  f.close();
  return true;
}

inline bool storageValidateRingFile(const char *path, uint16_t maxRec, HistHeader *outHdr = nullptr) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  HistHeader hdr;
  bool ok =
    f.size() == (size_t)(sizeof(HistHeader) + (size_t)maxRec * sizeof(HistRecord)) &&
    f.read((uint8_t*)&hdr, sizeof(hdr)) == sizeof(hdr) &&
    hdr.head < maxRec && hdr.count <= maxRec;
  f.close();
  if (ok && outHdr) *outHdr = hdr;
  return ok;
}

inline bool storageReplaceRingFile(const char *tmpPath, const char *dstPath, uint16_t maxRec) {
  if (!storageValidateRingFile(tmpPath, maxRec)) return false;
  LittleFS.remove(dstPath);
  if (LittleFS.rename(tmpPath, dstPath)) return true;
  // Fallback for filesystems that may fail rename across existing paths.
  File in = LittleFS.open(tmpPath, "r");
  if (!in) return false;
  File out = LittleFS.open(dstPath, "w");
  if (!out) { in.close(); return false; }
  uint8_t buf[256];
  while (in.available()) {
    yield();
    size_t n = in.read(buf, sizeof(buf));
    if (!n) break;
    if (out.write(buf, n) != n) { out.close(); in.close(); return false; }
  }
  out.close();
  in.close();
  bool ok = storageValidateRingFile(dstPath, maxRec);
  if (ok) LittleFS.remove(tmpPath);
  return ok;
}

inline void storageInit() {
  _storageInitRing(HIST_FILE, MAX_REC);
  _storageInitRing(HIST_RECENT_FILE, MAX_RECENT_REC);
}

inline void _storageWriteRing(const char *path, uint16_t maxRec, const SensorData &s) {
  File f = LittleFS.open(path, "r+");
  if (!f) { storageInit(); f = LittleFS.open(path, "r+"); }
  if (!f) return;

  HistHeader hdr;
  f.read((uint8_t*)&hdr, sizeof(hdr));
  if (hdr.head >= maxRec || hdr.count > maxRec) {
    f.close();
    _storageInitRing(path, maxRec);
    f = LittleFS.open(path, "r+");
    if (!f) return;
    f.read((uint8_t*)&hdr, sizeof(hdr));
  }

  HistRecord rec = { s.timestamp, s.level_pct, s.volume_liters, s.temp_c };
  uint32_t pos = sizeof(hdr) + hdr.head * sizeof(HistRecord);
  f.seek(pos);
  f.write((uint8_t*)&rec, sizeof(rec));

  hdr.head = (hdr.head + 1) % maxRec;
  if (hdr.count < maxRec) hdr.count++;

  f.seek(0);
  f.write((uint8_t*)&hdr, sizeof(hdr));
  f.close();
}

inline void storageWrite(const SensorData &s) {
  _storageWriteRing(HIST_FILE, MAX_REC, s);
}

inline void storageWriteRecent(const SensorData &s) {
  _storageWriteRing(HIST_RECENT_FILE, MAX_RECENT_REC, s);
}

// Read last `n` records (newest first) into out[]. Returns actual count.
inline int _storageReadRing(const char *path, uint16_t maxRec, HistRecord *out, int n) {
  File f = LittleFS.open(path, "r");
  if (!f) return 0;

  HistHeader hdr;
  if (f.size() != (size_t)(sizeof(HistHeader) + (size_t)maxRec * sizeof(HistRecord)) ||
      f.read((uint8_t*)&hdr, sizeof(hdr)) != sizeof(hdr) ||
      hdr.head >= maxRec || hdr.count > maxRec) {
    f.close();
    LittleFS.remove(path);
    _storageInitRing(path, maxRec);
    return 0;
  }
  if (hdr.count == 0) { f.close(); return 0; }

  int cnt = min((int)hdr.count, n);
  for (int i = 0; i < cnt; i++) {
    yield(); // avoid WDT on slow LittleFS seeks
    // Newest is (head-1), going backwards
    int idx = ((int)hdr.head - 1 - i + maxRec) % maxRec;
    f.seek(sizeof(hdr) + idx * sizeof(HistRecord));
    f.read((uint8_t*)&out[i], sizeof(HistRecord));
  }
  f.close();
  return cnt;
}

inline int storageRead(HistRecord *out, int n) {
  return _storageReadRing(HIST_FILE, MAX_REC, out, n);
}

inline int storageReadRecent(HistRecord *out, int n) {
  return _storageReadRing(HIST_RECENT_FILE, MAX_RECENT_REC, out, n);
}

inline uint16_t _storageCountRing(const char *path, uint16_t maxRec) {
  File f = LittleFS.open(path, "r");
  if (!f) return 0;
  HistHeader hdr;
  if (f.size() != (size_t)(sizeof(HistHeader) + (size_t)maxRec * sizeof(HistRecord)) ||
      f.read((uint8_t*)&hdr, sizeof(hdr)) != sizeof(hdr) ||
      hdr.head >= maxRec || hdr.count > maxRec) {
    f.close();
    LittleFS.remove(path);
    _storageInitRing(path, maxRec);
    return 0;
  }
  f.close();
  return hdr.count;
}

inline uint16_t storageCount() { return _storageCountRing(HIST_FILE, MAX_REC); }
inline uint16_t storageCountRecent() { return _storageCountRing(HIST_RECENT_FILE, MAX_RECENT_REC); }

inline void storageClear() {
  LittleFS.remove(HIST_FILE);
  LittleFS.remove(HIST_RECENT_FILE);
  storageInit();
}
