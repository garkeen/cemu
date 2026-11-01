#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host/host.h"

// A read-write image can be opened as a copy-on-write view (-snapshot): the
// base file is read-only and every touched 4KiB block is copied into a
// throwaway overlay before the write lands there. Reads consult the overlay
// first, so the run sees its own writes while the image on disk stays byte for
// byte what it was — the same contract QEMU's -snapshot gives, and the only
// way two boots can start from the same guest-visible disk contents after the
// first one wrote its boot flags and logs back.
enum { kSnapBlockLog2 = 12 };
enum { kSnapBlock = 1 << kSnapBlockLog2 };

struct HostFile {
  HANDLE handle;   // the file itself (read-only under -snapshot)
  HANDLE overlay;  // NULL unless this is a copy-on-write view
  uint8_t* dirty;  // one bit per 4KiB block of the overlay
  int64_t size;    // bytes in the base file
};

static int g_snapshot;

void HostFileSetSnapshot(int on) { g_snapshot = on; }

static size_t RawReadAt(HANDLE h, int64_t off, void* buf, size_t n) {
  LARGE_INTEGER pos;
  DWORD read = 0;
  pos.QuadPart = off;
  if (!SetFilePointerEx(h, pos, NULL, FILE_BEGIN)) return 0;
  if (!ReadFile(h, buf, (DWORD)n, &read, NULL)) return 0;
  return (size_t)read;
}

static size_t RawWriteAt(HANDLE h, int64_t off, const void* buf, size_t n) {
  LARGE_INTEGER pos;
  DWORD written = 0;
  pos.QuadPart = off;
  if (!SetFilePointerEx(h, pos, NULL, FILE_BEGIN)) return 0;
  if (!WriteFile(h, buf, (DWORD)n, &written, NULL)) return 0;
  return (size_t)written;
}

// Copies a block up on its first write, so a partial write never zeroes the
// rest of the block.
static int SnapTouch(HostFile* f, int64_t blk) {
  if (!f->dirty) return -1;
  if (f->dirty[blk >> 3] & (1u << (blk & 7))) return 0;
  uint8_t tmp[kSnapBlock];
  int64_t base = blk << kSnapBlockLog2;
  size_t n = (size_t)kSnapBlock;
  if (f->size - base < (int64_t)n) n = (size_t)(f->size - base);
  memset(tmp, 0, sizeof(tmp));
  if (n && RawReadAt(f->handle, base, tmp, n) != n) return -1;
  if (n && RawWriteAt(f->overlay, base, tmp, n) != n) return -1;
  f->dirty[blk >> 3] |= (uint8_t)(1u << (blk & 7));
  return 0;
}

static int SnapIsDirty(const HostFile* f, int64_t blk) {
  return f->dirty && (f->dirty[blk >> 3] & (1u << (blk & 7)));
}

static HostFile* OpenImage(const char* path, DWORD access) {
  HANDLE h = CreateFileA(path, access, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) return NULL;
  HostFile* f = (HostFile*)calloc(1, sizeof(HostFile));
  if (!f) {
    CloseHandle(h);
    return NULL;
  }
  f->handle = h;
  LARGE_INTEGER size;
  if (GetFileSizeEx(h, &size)) f->size = (int64_t)size.QuadPart;
  return f;
}

HostFile* HostFileOpenRead(const char* path) { return OpenImage(path, GENERIC_READ); }

HostFile* HostFileOpenReadWrite(const char* path) {
  HostFile* f = OpenImage(path, g_snapshot ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE));
  if (!f) return NULL;
  if (g_snapshot && f->size > 0) {
    char dir[MAX_PATH], tmp[MAX_PATH];
    DWORD n = GetTempPathA(sizeof(dir), dir);
    if (n && n < sizeof(dir)) {
      // PINNED: one overlay per process, deleted when the handle closes.
      snprintf(tmp, sizeof(tmp), "%scemu-snap-%lu.img", dir,
               (unsigned long)GetCurrentProcessId());
      HANDLE ov = CreateFileA(tmp, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
      if (ov != INVALID_HANDLE_VALUE) {
        int64_t blocks = (f->size + kSnapBlock - 1) >> kSnapBlockLog2;
        f->dirty = (uint8_t*)calloc((size_t)((blocks + 7) / 8), 1);
        if (f->dirty) {
          f->overlay = ov;
        } else {
          CloseHandle(ov);
        }
      }
    }
  }
  return f;
}

HostFile* HostFileCreate(const char* path) {
  // CREATE_ALWAYS truncates: a dump file's old contents are never wanted.
  HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) return NULL;
  HostFile* f = (HostFile*)calloc(1, sizeof(HostFile));
  if (!f) {
    CloseHandle(h);
    return NULL;
  }
  f->handle = h;
  return f;
}

int64_t HostFileSize(HostFile* f) {
  if (f->size) return f->size;
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

size_t HostFileReadAt(HostFile* f, int64_t off, void* buf, size_t n) {
  if (n == 0 || off < 0) return 0;
  if (!f->overlay) return RawReadAt(f->handle, off, buf, n);
  size_t total = 0;
  while (total < n) {
    int64_t pos = off + (int64_t)total;
    size_t within = (size_t)(pos & (kSnapBlock - 1));
    size_t chunk = kSnapBlock - within;
    if (chunk > n - total) chunk = n - total;
    HANDLE src = SnapIsDirty(f, pos >> kSnapBlockLog2) ? f->overlay : f->handle;
    size_t got = RawReadAt(src, pos, (uint8_t*)buf + total, chunk);
    if (got == 0) break;
    total += got;
    if (got < chunk) break;
  }
  return total;
}

size_t HostFileWriteAt(HostFile* f, int64_t off, const void* buf, size_t n) {
  if (n == 0 || off < 0) return 0;
  if (!f->overlay) return RawWriteAt(f->handle, off, buf, n);
  size_t total = 0;
  while (total < n) {
    int64_t pos = off + (int64_t)total;
    int64_t blk = pos >> kSnapBlockLog2;
    if ((uint64_t)pos >= (uint64_t)f->size) return total;
    if (SnapTouch(f, blk) != 0) return total;
    size_t within = (size_t)(pos & (kSnapBlock - 1));
    size_t chunk = kSnapBlock - within;
    if (chunk > n - total) chunk = n - total;
    size_t put = RawWriteAt(f->overlay, pos, (const uint8_t*)buf + total, chunk);
    if (put == 0) break;
    total += put;
    if (put < chunk) break;
  }
  return total;
}

void HostFileClose(HostFile* f) {
  if (!f) return;
  if (f->overlay) CloseHandle(f->overlay);
  free(f->dirty);
  CloseHandle(f->handle);
  free(f);
}

int HostFileRemove(const char* path) {
  return DeleteFileA(path) ? 0 : -1;
}
