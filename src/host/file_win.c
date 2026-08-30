#include <windows.h>
#include "host/host.h"

struct HostFile {
  HANDLE handle;
};

HostFile *HostFileOpenRead(const char *path) {
  HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) return NULL;
  HostFile *f = (HostFile *)malloc(sizeof(HostFile));
  if (!f) { CloseHandle(h); return NULL; }
  f->handle = h;
  return f;
}

int64_t HostFileSize(HostFile *f) {
  LARGE_INTEGER size;
  if (!GetFileSizeEx(f->handle, &size)) return -1;
  return (int64_t)size.QuadPart;
}

size_t HostFileRead(HostFile *f, void *buf, size_t n) {
  DWORD read = 0;
  if (n == 0) return 0;
  if (!ReadFile(f->handle, buf, (DWORD)n, &read, NULL)) return 0;
  return (size_t)read;
}

void HostFileClose(HostFile *f) {
  if (!f) return;
  CloseHandle(f->handle);
  free(f);
}
