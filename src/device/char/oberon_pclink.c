#include "device/char/oberon_pclink.h"

#include <stdio.h>
#include <string.h>

#include "util/log.h"

// Wire-protocol tags (pclink.c).
enum {
  kPclinkAck = 0x10,
  kPclinkRec = 0x21,
  kPclinkSnd = 0x22,
  kPclinkFrame = 256,   // payload frame: length byte plus up to 255 bytes
  kPclinkMaxFile = 0x1000000,
  kPclinkJobMax = 33,   // a job file holds a name of at most 31 chars
};

static const char kPclinkRecJob[] = "PCLink.REC";
static const char kPclinkSndJob[] = "PCLink.SND";

// A job file exists and carries a name: read it. Mirrors pclink.c GetJob —
// the name is the first whitespace-delimited token of at most 31 chars, and a
// job that cannot be taken is unlinked so it is not re-polled forever.
static int GetJob(OberonPclinkDevice* d, const char* job) {
  HostFile* f = HostFileOpenRead(job);
  if (!f) return 0;
  int64_t size = HostFileSize(f);
  if (size <= 0 || size > kPclinkJobMax) {
    HostFileClose(f);
    HostFileRemove(job);
    return 0;
  }
  char raw[kPclinkJobMax + 1];
  size_t n = HostFileRead(f, raw, (size_t)size);
  HostFileClose(f);
  if (n == 0 || sscanf(raw, "%31s", d->filename) != 1) {
    HostFileRemove(job);
    return 0;
  }
  d->txcount = 0;
  d->rxcount = 0;
  d->fnlen = (int)strlen(d->filename) + 1;
  return 1;
}

static void DropJob(OberonPclinkDevice* d, const char* job, int delete_payload) {
  if (delete_payload) HostFileRemove(d->filename);
  HostFileRemove(job);
  if (d->file) {
    HostFileClose(d->file);
    d->file = NULL;
  }
  d->mode = 0;
}

// Status read: also where an idle channel picks up the next job file
// (pclink.c PCLink_RStat).
uint32_t OberonPclinkReadStatus(OberonPclinkDevice* d) {
  if (d->mode == 0) {
    if (GetJob(d, kPclinkRecJob)) {
      HostFile* f = HostFileOpenRead(d->filename);
      if (f) {
        int64_t size = HostFileSize(f);
        if (size >= 0 && size < kPclinkMaxFile) {
          d->file = f;
          d->file_pos = 0;
          d->flen = (int)size;
          d->mode = kPclinkRec;
          LogInfo("pclink rec: %s (%d bytes)", d->filename, d->flen);
        } else {
          HostFileClose(f);
        }
      }
      if (d->mode == 0) HostFileRemove(kPclinkRecJob);
    } else if (GetJob(d, kPclinkSndJob)) {
      HostFile* f = HostFileCreate(d->filename);
      if (f) {
        d->file = f;
        d->file_pos = 0;
        d->flen = -1;
        d->mode = kPclinkSnd;
        LogInfo("pclink snd: %s", d->filename);
      }
      if (d->mode == 0) HostFileRemove(kPclinkSndJob);
    }
  }
  return 2 + (d->mode != 0);  // bit 1: transmit always ready
}

uint32_t OberonPclinkReadData(OberonPclinkDevice* d) {
  uint8_t ch = 0;
  if (d->mode != 0) {
    if (d->rxcount == 0) {
      ch = d->mode;  // the REC/SND tag
    } else if (d->rxcount < d->fnlen + 1) {
      ch = (uint8_t)d->filename[d->rxcount - 1];  // name bytes, NUL included
    } else if (d->mode == kPclinkSnd) {
      ch = kPclinkAck;
      if (d->flen == 0) {  // a short frame ended the payload: job done
        d->mode = 0;
        HostFileRemove(kPclinkSndJob);
      }
    } else {
      int pos = (d->rxcount - d->fnlen - 1) % kPclinkFrame;
      if (pos == 0 || d->flen == 0) {
        // Frame boundary: its length, or the end marker when nothing is left.
        if (d->flen > 255) {
          ch = 255;
        } else {
          ch = (uint8_t)d->flen;
          if (d->flen == 0) {
            d->mode = 0;
            HostFileRemove(kPclinkRecJob);
          }
        }
      } else {
        if (d->file) HostFileReadAt(d->file, d->file_pos++, &ch, 1);
        d->flen--;
      }
    }
  }
  d->rxcount++;
  return ch;
}

void OberonPclinkWriteData(OberonPclinkDevice* d, uint32_t value) {
  if (d->mode != 0) {
    if (d->txcount == 0) {
      if (value != kPclinkAck) {
        // The guest refused the job: close and clean up, deleting a payload
        // file a send had already created (pclink.c PCLink_TData).
        DropJob(d, d->mode == kPclinkSnd ? kPclinkSndJob : kPclinkRecJob,
                d->mode == kPclinkSnd);
      }
    } else if (d->mode == kPclinkSnd) {
      int pos = (d->txcount - 1) % kPclinkFrame;
      d->buf[pos] = (uint8_t)value;
      int lim = d->buf[0];
      if (pos == lim) {  // frame complete: buf[0] was its length byte
        if (d->file) HostFileWriteAt(d->file, d->file_pos, d->buf + 1, (size_t)lim);
        d->file_pos += lim;
        if (lim < 255) {  // short frame: end of file
          d->flen = 0;
          if (d->file) {
            HostFileClose(d->file);
            d->file = NULL;
          }
        }
      }
    }
  }
  d->txcount++;
}

void OberonPclinkInit(OberonPclinkDevice* d) { memset(d, 0, sizeof(*d)); }

void OberonPclinkDestroy(OberonPclinkDevice* d) {
  if (d->file) {
    HostFileClose(d->file);
    d->file = NULL;
  }
}
