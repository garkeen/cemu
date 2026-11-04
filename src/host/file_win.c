#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host/host.h"

#include "util/log.h"

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
  int snapshot;    // opened under -snapshot: the base file is read-only
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

// A directory the overlay can be created in. GetTempPathA returns TMP even
// when the directory does not exist, and CreateFileA then fails; checking the
// attributes and trying TEMP next is what keeps a stale TMP from turning the
// whole guest disk read-only (QEMU's create_tmp_file fails loudly instead:
// block.c "Could not open temporary file"). The image's own directory and the
// current directory are the last resorts -- both are places the caller could
// already read the image from.
static int DirExists(const char* path) {
  DWORD a = GetFileAttributesA(path);
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static int EnvDir(const char* name, char* out, size_t cap) {
  DWORD n = GetEnvironmentVariableA(name, out, (DWORD)cap);
  if (n == 0 || n >= cap) return 0;
  return DirExists(out);
}

static int ImageDir(const char* path, char* out, size_t cap) {
  const char* slash = strrchr(path, '\\');
  const char* fslash = strrchr(path, '/');
  if (!slash || (fslash && fslash > slash)) slash = fslash;
  if (!slash) return 0;
  size_t len = (size_t)(slash - path) + 1;
  if (len >= cap) return 0;
  memcpy(out, path, len);
  out[len] = 0;
  return 1;
}

static int PickOverlayDir(const char* image, char* out, size_t cap) {
  if (EnvDir("TMP", out, cap)) return 1;
  if (EnvDir("TEMP", out, cap)) return 1;
  if (ImageDir(image, out, cap) && DirExists(out)) return 1;
  DWORD n = GetCurrentDirectoryA((DWORD)cap, out);
  return n > 0 && n < cap;
}

HostFile* HostFileOpenReadWrite(const char* path) {
  HostFile* f = OpenImage(path, g_snapshot ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE));
  if (!f) return NULL;
  f->snapshot = g_snapshot;
  if (g_snapshot) {
    // A snapshot's base file is read-only, so without an overlay every write
    // would fail. Refuse the open instead: a silent read-only disk is a data
    // loss trap the guest cannot report (Win95 pages its own code from disk
    // and dies later with an unrelated-looking blue screen).
    char dir[MAX_PATH], tmp[MAX_PATH];
    if (!PickOverlayDir(path, dir, sizeof(dir))) {
      LogError("snapshot: no usable temp directory for the copy-on-write overlay");
      CloseHandle(f->handle);
      free(f);
      return NULL;
    }
    size_t len = strlen(dir);
    if (len && dir[len - 1] != '\\' && dir[len - 1] != '/') {
      if (len + 1 >= sizeof(dir)) {
        LogError("snapshot: temp directory path too long ('%s')", dir);
        CloseHandle(f->handle);
        free(f);
        return NULL;
      }
      dir[len++] = '\\';
      dir[len] = 0;
    }
    // PINNED: one overlay per process, deleted when the handle closes. The
    // sequence number keeps a machine with several images (xv6's -hda + -hdb,
    // a CD-ROM beside a disk) from reusing the same path: the second
    // CreateFileA would hit the first handle's share mode and fail with
    // ERROR_SHARING_VIOLATION.
    static unsigned s_overlay_seq;
    snprintf(tmp, sizeof(tmp), "%scemu-snap-%lu-%u.img", dir,
             (unsigned long)GetCurrentProcessId(), ++s_overlay_seq);
    HANDLE ov = CreateFileA(tmp, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (ov == INVALID_HANDLE_VALUE) {
      LogError("snapshot: cannot create copy-on-write overlay '%s' (error %lu)", tmp,
               (unsigned long)GetLastError());
      CloseHandle(f->handle);
      free(f);
      return NULL;
    }
    int64_t blocks = (f->size + kSnapBlock - 1) >> kSnapBlockLog2;
    size_t map = (size_t)((blocks + 7) / 8);
    f->dirty = (uint8_t*)calloc(map ? map : 1, 1);
    if (!f->dirty) {
      LogError("snapshot: out of memory for the overlay bitmap");
      CloseHandle(ov);
      CloseHandle(f->handle);
      free(f);
      return NULL;
    }
    f->overlay = ov;
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
  if (!f->overlay) {
    // Under -snapshot the base handle is read-only: a write here means the
    // overlay was never installed, and it must fail rather than fall through
    // to the image (open refuses that case; this is the last line of defense).
    if (f->snapshot) return 0;
    return RawWriteAt(f->handle, off, buf, n);
  }
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
