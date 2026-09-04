#include "util/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "host/host.h"

static FILE* g_log_file = NULL;

void LogInitFile(const char* path) {
  if (path) g_log_file = fopen(path, "w");
}

static void Emit(const char* buf, size_t n) {
  HostWriteErr(buf, n);
  if (g_log_file) fwrite(buf, 1, n, g_log_file);
}

static void VLog(const char* tag, const char* fmt, va_list ap) {
  char buf[1024];
  int n = snprintf(buf, sizeof(buf), "[%s] ", tag);
  n += vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
  if (n > 0 && (size_t)n < sizeof(buf)) Emit(buf, (size_t)n);
}

void LogInfo(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  VLog("info", fmt, ap);
  va_end(ap);
}

void LogError(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  VLog("error", fmt, ap);
  va_end(ap);
}

void Fatal(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  VLog("fatal", fmt, ap);
  va_end(ap);
  exit(1);
}
