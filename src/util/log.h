#ifndef CEMU_UTIL_LOG_H
#define CEMU_UTIL_LOG_H

void LogInitFile(const char *path);
void LogInfo(const char *fmt, ...);
void LogError(const char *fmt, ...);
void Fatal(const char *fmt, ...);

#endif
