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

// Alert state (avoid sending repeated alerts)
static bool _alertLowSent  = false;
static bool _alertHighSent = false;

// ------------------------------------------------------------------
inline void tgSetup(const Config &c) {
  if (!c.tg_en || strlen(c.tg_token) < 10) return;
  _tgClient.setInsecure();   // skip cert check — saves memory on ESP8266
  _tgBot = new UniversalTelegramBot(c.tg_token, _tgClient);
  _tgEnabled = true;
  Serial.println(F("[TG] Telegram enabled"));
}

inline void tgSend(const Config &c, const String &msg) {
  if (!_tgEnabled || !_tgBot || strlen(c.tg_chat) == 0) return;
  _tgBot->sendMessage(c.tg_chat, msg, "Markdown");
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
  m += F("🕒 Замер: ");
  struct tm *ti = localtime((time_t*)&s.timestamp);
  char buf[20];
  strftime(buf, sizeof(buf), "%d.%m %H:%M", ti);
  m += buf;
  return m;
}

// ------------------------------------------------------------------
// Poll for new messages and respond
// ------------------------------------------------------------------
inline void tgLoop(const Config &c, const SensorData &s) {
  if (!_tgEnabled || !_tgBot) return;

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

    } else if (txt == "/status" || txt == "/статус") {
      tgSend(c, _statusMsg(c, s));

    } else if (txt == "/start" || txt == "/help" || txt == "/помощь") {
      String h = F("*WaterSense Bot*\n\n");
      h += F("/level — текущий уровень\n");
      h += F("/status — полный статус\n");
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

  if (s.level_pct < c.tg_alert_low && !_alertLowSent) {
    String m = F("⚠️ *Мало воды!*\nУровень: *");
    m += String(s.level_pct, 1); m += F("%* (порог ");
    m += String(c.tg_alert_low, 0); m += F("%)");
    tgSend(c, m);
    _alertLowSent  = true;
    _alertHighSent = false;
  } else if (s.level_pct >= c.tg_alert_low + 5.0f) {
    _alertLowSent = false;  // reset after recovery
  }

  if (s.level_pct > c.tg_alert_high && !_alertHighSent) {
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

inline bool tgEnabled() { return _tgEnabled; }
