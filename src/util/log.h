#ifndef CEMU_UTIL_LOG_H
#define CEMU_UTIL_LOG_H

// The host's own failures exit with an even status. A guest status reaches the
// host through the debug-exit device as (value << 1) | 1 (AGENTS.md 裁决存档,
// isa-debug-exit), so it is always odd — an even status can never be read as a
// guest-reported pass (1) or failure (3, 5, ...). Without the distinction a
// host failure as ordinary as a missing image file, or a triple fault, looks
// exactly like a pass to any harness that judges by exit status.
enum { kExitHostFailure = 2 };

void LogInitFile(const char* path);
void LogInfo(const char* fmt, ...);
void LogError(const char* fmt, ...);
void Fatal(const char* fmt, ...);

#endif
