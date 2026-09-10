#include "log.h"
#include <stdarg.h>

namespace {
bool isMuted = false;
}

namespace Log {

void setMuted(bool m) { isMuted = m; }
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
