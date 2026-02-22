#pragma once
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include "config.h"
#include "sensor.h"

static WiFiClientSecure   _tgClient;
static UniversalTelegramBot *_tgBot = nullptr;
static unsigned long       _tgLastMsgId = 0;
static bool                _tgEnabled   = false;
static bool                _tgBacklogSynced = false;
static void              (*_tgMeasureCallback)() = nullptr;

// Alert state (avoid sending repeated alerts)
static bool _alertLowSent  = false;
static bool _alertHighSent = false;

// ------------------------------------------------------------------
inline void tgSetup(const Config &c) {
  if (!c.tg_en || strlen(c.tg_token) < 10) return;
  _tgClient.setInsecure();   // skip cert check — saves memory on ESP8266
  _tgClient.setBufferSizes(1024, 512); // reduce BearSSL RAM usage on ESP8266
  _tgBot = new UniversalTelegramBot(c.tg_token, _tgClient);
  _tgBot->longPoll = 0;          // avoid long blocking calls in loop()
  _tgBot->waitForResponse = 1500;
  _tgBot->maxMessageLength = 1200;
  _tgLastMsgId = 0;
  _tgBacklogSynced = false;
  _tgEnabled = true;
  dbgPrintln(F("[TG] Telegram enabled"));
}

inline void tgSend(const Config &c, const String &msg) {
  if (!_tgEnabled || !_tgBot || strlen(c.tg_chat) == 0) return;
  _tgBot->sendMessage(c.tg_chat, msg, "Markdown");
}

inline void tgSetMeasureCallback(void (*cb)()) {
  _tgMeasureCallback = cb;
}

// ------------------------------------------------------------------
// Build status string
// ------------------------------------------------------------------
static String _statusMsg(const Config &c, const SensorData &s) {
  String m = String(F("*Уровень воды*\n"));
  m += F("📊 Уровень: *"); m += String(s.level_pct, 1); m += F("%*\n");
  m += F("📏 Расстояние: "); m += String(s.distance_cm, 1); m += F(" см\n");
  if (c.barrel_diam_cm > 0) {
    m += F("🪣 Объём: "); m += String(s.volume_liters, 1); m += F(" л\n");
    m += F("⬜ Свободно: "); m += String(s.free_liters, 1); m += F(" л\n");
  }
  if (!isnan(s.temp_c)) {
    m += F("🌡 Температура: "); m += String(s.temp_c, 1); m += F(" °C\n");
  }
  m += F("🕒 Замер: ");
  time_t ts = (time_t)s.timestamp;
  struct tm *ti = localtime(&ts);
  char buf[20];
  if (ti) {
    strftime(buf, sizeof(buf), "%d.%m %H:%M", ti);
    m += buf;
  } else {
    m += F("—");
  }
  return m;
}

// ------------------------------------------------------------------
// Poll for new messages and respond
// ------------------------------------------------------------------
inline void tgLoop(const Config &c, const SensorData &s) {
  if (!_tgEnabled || !_tgBot || !c.tg_cmd_en) return;

  // On first poll after boot, skip any old backlog so the device does not try
  // to process stale commands accumulated while it was offline.
  if (!_tgBacklogSynced) {
    int n0 = _tgBot->getUpdates(-1); // fetch at most the latest update (HANDLE_MESSAGES=1)
    if (n0 > 0) {
      telegramMessage &m0 = _tgBot->messages[n0 - 1];
      _tgLastMsgId = m0.update_id;
      dbgPrintf("[TG] Backlog synced to update_id=%lu\n", _tgLastMsgId);
    } else {
      dbgPrintln(F("[TG] Backlog sync: no pending updates"));
    }
    _tgBacklogSynced = true;
    return;
  }

  int n = _tgBot->getUpdates(_tgLastMsgId + 1);
  for (int i = 0; i < n; i++) {
    telegramMessage &msg = _tgBot->messages[i];
    _tgLastMsgId = msg.update_id;

    // Accept only from configured chat
    if (strlen(c.tg_chat) && msg.chat_id != String(c.tg_chat)) continue;

    String txt = msg.text;
    txt.toLowerCase();

    if (txt == "/level" || txt == "/уровень") {
      String r = F("📊 Уровень: *"); r += String(s.level_pct, 1); r += F("%*");
      if (c.barrel_diam_cm > 0) { r += F("\n🪣 "); r += String(s.volume_liters, 1); r += F(" л"); }
      tgSend(c, r);

    } else if (txt == "/measure" || txt == "/замер" || txt == "/update" || txt == "/обновить") {
      if (_tgMeasureCallback) _tgMeasureCallback();
      tgSend(c, _statusMsg(c, s));

    } else if (txt == "/status" || txt == "/статус") {
      tgSend(c, _statusMsg(c, s));

    } else if (txt == "/start" || txt == "/help" || txt == "/помощь") {
      String h = F("*WaterSense Bot*\n\n");
      h += F("/level — текущий уровень\n");
      h += F("/status — полный статус\n");
      h += F("/measure — новый замер сейчас\n");
      h += F("/help — эта справка\n\n");
      h += F("🌐 Веб-интерфейс: http://");
      h += WiFi.localIP().toString();
      tgSend(c, h);

    } else {
      tgSend(c, F("Неизвестная команда. /help — список команд."));
    }
  }
}

// ------------------------------------------------------------------
// Check thresholds and send alert if needed
// ------------------------------------------------------------------
inline void tgCheckAlerts(const Config &c, const SensorData &s) {
  if (!_tgEnabled || !s.valid) return;
  if (!c.tg_alert_low_en) _alertLowSent = false;
  if (!c.tg_alert_high_en) _alertHighSent = false;

  if (c.tg_alert_low_en && s.level_pct < c.tg_alert_low && !_alertLowSent) {
    String m = F("⚠️ *Мало воды!*\nУровень: *");
    m += String(s.level_pct, 1); m += F("%* (порог ");
    m += String(c.tg_alert_low, 0); m += F("%)");
    tgSend(c, m);
    _alertLowSent  = true;
    _alertHighSent = false;
  } else if (s.level_pct >= c.tg_alert_low + 5.0f) {
    _alertLowSent = false;  // reset after recovery
  }

  if (c.tg_alert_high_en && s.level_pct > c.tg_alert_high && !_alertHighSent) {
    String m = F("🔵 *Много воды!*\nУровень: *");
    m += String(s.level_pct, 1); m += F("%* (порог ");
    m += String(c.tg_alert_high, 0); m += F("%)");
    tgSend(c, m);
    _alertHighSent = true;
    _alertLowSent  = false;
  } else if (s.level_pct <= c.tg_alert_high - 5.0f) {
    _alertHighSent = false;
  }
}

// ------------------------------------------------------------------
// Daily summary (call at midnight)
// ------------------------------------------------------------------
inline void tgDailySummary(const Config &c, const SensorData &s) {
  if (!_tgEnabled || !c.tg_daily) return;
  String m = F("📅 *Ежедневный отчёт*\n");
  m += _statusMsg(c, s);
  tgSend(c, m);
}

inline void tgBootMessage(const Config &c, const SensorData &s) {
  if (!_tgEnabled || !c.tg_boot_msg_en) return;
  String m = F("🚀 *WaterSense запущен*\n");
  m += _statusMsg(c, s);
  m += F("\n🌐 Веб: http://");
  m += WiFi.localIP().toString();
  m += F("\n💬 Команда: /measure");
  tgSend(c, m);
}

inline bool tgEnabled() { return _tgEnabled; }
