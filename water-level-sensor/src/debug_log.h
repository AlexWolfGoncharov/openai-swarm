#pragma once
#include <Arduino.h>
#include <stdarg.h>

#ifndef DBG_LOG_LINES
#define DBG_LOG_LINES 32
#endif

#ifndef DBG_LOG_LINE_LEN
#define DBG_LOG_LINE_LEN 160
#endif

static char     _dbgLines[DBG_LOG_LINES][DBG_LOG_LINE_LEN];
static uint8_t  _dbgHead = 0;   // next write slot
static uint8_t  _dbgCount = 0;  // committed lines
static char     _dbgCur[DBG_LOG_LINE_LEN];
static uint16_t _dbgCurLen = 0; // pending line (until '\n')

inline void _dbgCommitCur() {
  _dbgCur[_dbgCurLen] = '\0';
  strlcpy(_dbgLines[_dbgHead], _dbgCur, DBG_LOG_LINE_LEN);
  _dbgHead = (_dbgHead + 1) % DBG_LOG_LINES;
  if (_dbgCount < DBG_LOG_LINES) _dbgCount++;
  _dbgCurLen = 0;
  _dbgCur[0] = '\0';
}

inline void _dbgFeedChar(char ch) {
  if (ch == '\r') return;
  if (ch == '\n') {
    _dbgCommitCur();
    return;
  }
  if (_dbgCurLen < DBG_LOG_LINE_LEN - 1) {
    _dbgCur[_dbgCurLen++] = ch;
    return;
  }
  // Truncate and commit when the line is too long.
  _dbgCur[DBG_LOG_LINE_LEN - 2] = '~';
  _dbgCurLen = DBG_LOG_LINE_LEN - 1;
  _dbgCommitCur();
}

inline void dbgFeed(const char *s) {
  if (!s) return;
  while (*s) _dbgFeedChar(*s++);
}

inline void dbgPrint(const char *s) {
  Serial.print(s);
  dbgFeed(s);
}

inline void dbgPrint(const String &s) {
  Serial.print(s);
  dbgFeed(s.c_str());
}

inline void dbgPrint(const __FlashStringHelper *s) {
  Serial.print(s);
  String tmp(s);
  dbgFeed(tmp.c_str());
}

inline void dbgPrintln(const char *s) {
  Serial.println(s);
  dbgFeed(s);
  dbgFeed("\n");
}

inline void dbgPrintln(const String &s) {
  Serial.println(s);
  dbgFeed(s.c_str());
  dbgFeed("\n");
}

inline void dbgPrintln(const __FlashStringHelper *s) {
  Serial.println(s);
  String tmp(s);
  dbgFeed(tmp.c_str());
  dbgFeed("\n");
}

inline void dbgPrintln() {
  Serial.println();
  dbgFeed("\n");
}

inline int dbgPrintf(const char *fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n < 0) return n;
  Serial.print(buf);
  dbgFeed(buf);
  return n;
}

inline uint8_t dbgLogCount() { return _dbgCount; }

inline String dbgLogLineAt(uint8_t idx) {
  if (idx >= _dbgCount) return String();
  uint8_t start = (_dbgHead + DBG_LOG_LINES - _dbgCount) % DBG_LOG_LINES;
  uint8_t pos = (start + idx) % DBG_LOG_LINES;
  return String(_dbgLines[pos]);
}

