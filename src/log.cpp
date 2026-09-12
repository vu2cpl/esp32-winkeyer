#include "log.h"
#include <esp_log.h>
#include <stdarg.h>

namespace {
bool isMuted = false;
}

namespace Log {

void setMuted(bool m) {
  isMuted = m;
  // Keep the core's logger in step: it writes to the same UART without
  // passing through here, so muting one and not the other leaks.
  esp_log_level_set("*", m ? ESP_LOG_NONE : ESP_LOG_ERROR);
}
bool muted()          { return isMuted; }

void printf(const char* fmt, ...) {
  if (isMuted) return;
  va_list ap;
  va_start(ap, fmt);
  char buf[192];
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  Serial.print(buf);
}

void println(const char* s) {
  if (isMuted) return;
  Serial.println(s);
}

}  // namespace Log
