#include <windows.h>

#include "host/host.h"

struct HostFile {
  HANDLE handle;
};

static HostFile* OpenImage(const char* path, DWORD access) {
  HANDLE h = CreateFileA(path, access, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) return NULL;
  HostFile* f = (HostFile*)malloc(sizeof(HostFile));
  if (!f) {
    CloseHandle(h);
    return NULL;
  }
  f->handle = h;
  return f;
}

HostFile* HostFileOpenRead(const char* path) { return OpenImage(path, GENERIC_READ); }

HostFile* HostFileOpenReadWrite(const char* path) {
  return OpenImage(path, GENERIC_READ | GENERIC_WRITE);
}

HostFile* HostFileCreate(const char* path) {
  // CREATE_ALWAYS truncates: a dump file's old contents are never wanted.
  HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) return NULL;
  HostFile* f = (HostFile*)malloc(sizeof(HostFile));
  if (!f) {
    CloseHandle(h);
    return NULL;
  }
  f->handle = h;
  return f;
}

int64_t HostFileSize(HostFile* f) {
  LARGE_INTEGER size;
  if (!GetFileSizeEx(f->handle, &size)) return -1;
  return (int64_t)size.QuadPart;
}

size_t HostFileRead(HostFile* f, void* buf, size_t n) {
  DWORD read = 0;
  if (n == 0) return 0;
  if (!ReadFile(f->handle, buf, (DWORD)n, &read, NULL)) return 0;
  return (size_t)read;
}

static int SeekTo(HostFile* f, int64_t off) {
  LARGE_INTEGER pos;
  pos.QuadPart = off;
  return SetFilePointerEx(f->handle, pos, NULL, FILE_BEGIN) ? 0 : -1;
}

size_t HostFileReadAt(HostFile* f, int64_t off, void* buf, size_t n) {
  DWORD read = 0;
  if (n == 0 || off < 0) return 0;
  if (SeekTo(f, off) != 0) return 0;
  if (!ReadFile(f->handle, buf, (DWORD)n, &read, NULL)) return 0;
  return (size_t)read;
}

size_t HostFileWriteAt(HostFile* f, int64_t off, const void* buf, size_t n) {
  DWORD written = 0;
  if (n == 0 || off < 0) return 0;
  if (SeekTo(f, off) != 0) return 0;
  if (!WriteFile(f->handle, buf, (DWORD)n, &written, NULL)) return 0;
  return (size_t)written;
}

void HostFileClose(HostFile* f) {
  if (!f) return;
  CloseHandle(f->handle);
  free(f);
}
