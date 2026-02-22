#pragma once
#include <Arduino.h>
#include <LittleFS.h>
#include "sensor.h"

// Circular buffer of hourly snapshots stored in LittleFS
// Format:  header(4 bytes)  + MAX_REC * Record(16 bytes)
//   header: uint16 head, uint16 count
//   Record: uint32 ts + float level_pct + float volume_liters + float temp_c

#define HIST_FILE   "/hist.bin"
#define MAX_REC     168   // 7 days × 24 h

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

inline void storageInit() {
  // Recreate history if file format/size changed (e.g. firmware upgrade).
  if (LittleFS.exists(HIST_FILE)) {
    File f = LittleFS.open(HIST_FILE, "r");
    bool valid = false;
    if (f) {
      HistHeader hdr;
      if (f.size() == (size_t)(sizeof(HistHeader) + MAX_REC * sizeof(HistRecord)) &&
          f.read((uint8_t*)&hdr, sizeof(hdr)) == sizeof(hdr) &&
          hdr.head < MAX_REC && hdr.count <= MAX_REC) {
        valid = true;
      }
      f.close();
    }
    if (valid) return;
    LittleFS.remove(HIST_FILE);
  }
  // Create blank file
  File f = LittleFS.open(HIST_FILE, "w");
  if (!f) return;
  HistHeader hdr = {0, 0};
  f.write((uint8_t*)&hdr, sizeof(hdr));
  // Pre-allocate space
  HistRecord blank = {0, 0, 0, NAN};
  for (int i = 0; i < MAX_REC; i++) f.write((uint8_t*)&blank, sizeof(blank));
  f.close();
}

inline void storageWrite(const SensorData &s) {
  File f = LittleFS.open(HIST_FILE, "r+");
  if (!f) { storageInit(); f = LittleFS.open(HIST_FILE, "r+"); }
  if (!f) return;

  HistHeader hdr;
  f.read((uint8_t*)&hdr, sizeof(hdr));

  HistRecord rec = { s.timestamp, s.level_pct, s.volume_liters, s.temp_c };
  uint32_t pos = sizeof(hdr) + hdr.head * sizeof(HistRecord);
  f.seek(pos);
  f.write((uint8_t*)&rec, sizeof(rec));

  hdr.head = (hdr.head + 1) % MAX_REC;
  if (hdr.count < MAX_REC) hdr.count++;

  f.seek(0);
  f.write((uint8_t*)&hdr, sizeof(hdr));
  f.close();
}

// Read last `n` records (newest first) into out[]. Returns actual count.
inline int storageRead(HistRecord *out, int n) {
  File f = LittleFS.open(HIST_FILE, "r");
  if (!f) return 0;

  HistHeader hdr;
  if (f.size() != (size_t)(sizeof(HistHeader) + MAX_REC * sizeof(HistRecord)) ||
      f.read((uint8_t*)&hdr, sizeof(hdr)) != sizeof(hdr) ||
      hdr.head >= MAX_REC || hdr.count > MAX_REC) {
    f.close();
    LittleFS.remove(HIST_FILE);
    storageInit();
    return 0;
  }
  if (hdr.count == 0) { f.close(); return 0; }

  int cnt = min((int)hdr.count, n);
  for (int i = 0; i < cnt; i++) {
    // Newest is (head-1), going backwards
    int idx = ((int)hdr.head - 1 - i + MAX_REC) % MAX_REC;
    f.seek(sizeof(hdr) + idx * sizeof(HistRecord));
    f.read((uint8_t*)&out[i], sizeof(HistRecord));
  }
  f.close();
  return cnt;
}

inline uint16_t storageCount() {
  File f = LittleFS.open(HIST_FILE, "r");
  if (!f) return 0;
  HistHeader hdr;
  if (f.size() != (size_t)(sizeof(HistHeader) + MAX_REC * sizeof(HistRecord)) ||
      f.read((uint8_t*)&hdr, sizeof(hdr)) != sizeof(hdr) ||
      hdr.head >= MAX_REC || hdr.count > MAX_REC) {
    f.close();
    LittleFS.remove(HIST_FILE);
    storageInit();
    return 0;
  }
  f.close();
  return hdr.count;
}

inline void storageClear() {
  LittleFS.remove(HIST_FILE);
  storageInit();
}
