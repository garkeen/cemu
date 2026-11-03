// Win32 PCM output backend (host/ is the only windows.h user). One waveOut
// stream, filled from the run loop: the SB16 device (device/audio/sb16.c) asks
// for free space, converts its own sample format and queues the bytes. No
// callback thread and no cross-thread state -- waveOut's driver marks a header
// WHDR_DONE when it has played, and HostAudioFree simply counts the headers
// that are not in the queue.
#include <windows.h>

#include <mmsystem.h>

#include <stdlib.h>
#include <string.h>

#include "host/host.h"
#include "util/log.h"

// Small blocks keep the SB16's IRQ timing close to the sample clock: at
// 44.1 kHz stereo 16-bit one 512-byte block is 2.9 ms, at 11 kHz mono 8-bit it
// is 46 ms. Sixteen of them give 8 KiB of slack.
enum {
  kHostAudioBuffers = 16,
  kHostAudioBufferBytes = 512,
};

struct HostAudio {
  HWAVEOUT handle;
  WAVEHDR header[kHostAudioBuffers];
  uint8_t* data[kHostAudioBuffers];
  int channels;
  int bits;
  int is_signed;
  int bytes_per_frame;
  int write_failed;  // the driver's first waveOutWrite failure is reported once
};

// waveOut only takes unsigned 8-bit and signed 16-bit PCM. The SB16 can also
// ask for signed 8-bit and unsigned 16-bit (the DSP format byte's bit 4), so
// the conversion is done while copying into the waveOut block.
static void ConvertSamples(const HostAudio* a, uint8_t* dst, const uint8_t* src, int len) {
  if (a->bits == 8) {
    if (!a->is_signed) {
      memcpy(dst, src, (size_t)len);
      return;
    }
    for (int i = 0; i < len; i++) dst[i] = (uint8_t)(src[i] ^ 0x80);
    return;
  }
  if (a->is_signed) {
    memcpy(dst, src, (size_t)len);
    return;
  }
  // 16-bit unsigned -> signed: flip the sign bit, i.e. the high byte on the
  // little-endian host (WAVE_FORMAT_PCM is little-endian).
  for (int i = 0; i + 1 < len; i += 2) {
    dst[i] = src[i];
    dst[i + 1] = (uint8_t)(src[i + 1] ^ 0x80);
  }
  if (len & 1) dst[len - 1] = src[len - 1];
}

HostAudio* HostAudioOpen(int rate, int channels, int bits, int is_signed) {
  if (rate <= 0 || (channels != 1 && channels != 2) || (bits != 8 && bits != 16)) return NULL;
  HostAudio* a = (HostAudio*)calloc(1, sizeof(*a));
  if (!a) return NULL;
  a->channels = channels;
  a->bits = bits;
  a->is_signed = is_signed;
  a->bytes_per_frame = channels * (bits / 8);

  WAVEFORMATEX fmt;
  memset(&fmt, 0, sizeof(fmt));
  fmt.wFormatTag = WAVE_FORMAT_PCM;
  fmt.nChannels = (WORD)channels;
  fmt.nSamplesPerSec = (DWORD)rate;
  fmt.wBitsPerSample = (WORD)bits;
  fmt.nBlockAlign = (WORD)a->bytes_per_frame;
  fmt.nAvgBytesPerSec = (DWORD)(rate * a->bytes_per_frame);
  MMRESULT rc = waveOutOpen(&a->handle, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL);
  if (rc != MMSYSERR_NOERROR) {
    LogInfo("host audio: waveOutOpen failed (%u), running silent", (unsigned)rc);
    free(a);
    return NULL;
  }
  WAVEOUTCAPSA caps;
  if (waveOutGetDevCaps(WAVE_MAPPER, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
    LogInfo("host audio: device '%s'", caps.szPname);
  DWORD volume = 0;
  if (waveOutGetVolume(a->handle, &volume) == MMSYSERR_NOERROR)
    LogInfo("host audio: volume=%08x", (unsigned)volume);
  LogInfo("host audio: %d Hz, %d channel(s), %d-bit%s", rate, channels, bits,
          is_signed ? " signed" : " unsigned");

  for (int i = 0; i < kHostAudioBuffers; i++) {
    a->data[i] = (uint8_t*)malloc(kHostAudioBufferBytes);
    if (!a->data[i]) {
      HostAudioClose(a);
      return NULL;
    }
    memset(&a->header[i], 0, sizeof(a->header[i]));
    a->header[i].lpData = (LPSTR)a->data[i];
    a->header[i].dwBufferLength = kHostAudioBufferBytes;
    if (waveOutPrepareHeader(a->handle, &a->header[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
      HostAudioClose(a);
      return NULL;
    }
  }
  return a;
}

int HostAudioFree(const HostAudio* a) {
  if (!a) return 0;
  int free_bytes = 0;
  for (int i = 0; i < kHostAudioBuffers; i++) {
    // WHDR_INQUEUE is cleared by the driver when the block has been played
    // (WHDR_DONE is then set); a header that was never written is free too.
    if (!(a->header[i].dwFlags & WHDR_INQUEUE)) free_bytes += kHostAudioBufferBytes;
  }
  return free_bytes;
}

int HostAudioWrite(HostAudio* a, const void* buf, int len) {
  if (!a || !buf || len <= 0) return 0;
  const uint8_t* src = (const uint8_t*)buf;
  int done = 0;
  while (done < len) {
    int slot = -1;
    for (int i = 0; i < kHostAudioBuffers; i++) {
      if (!(a->header[i].dwFlags & WHDR_INQUEUE)) {
        slot = i;
        break;
      }
    }
    if (slot < 0) break;
    int n = len - done;
    if (n > kHostAudioBufferBytes) n = kHostAudioBufferBytes;
    // A block may not split a sample frame; the SB16 only asks for whole
    // frames, and the block size is a multiple of every frame size.
    if (n >= a->bytes_per_frame) n -= n % a->bytes_per_frame;
    ConvertSamples(a, a->data[slot], src + done, n);
    a->header[slot].dwBufferLength = (DWORD)n;
    MMRESULT rc = waveOutWrite(a->handle, &a->header[slot], sizeof(WAVEHDR));
    if (rc != MMSYSERR_NOERROR) {
      if (!a->write_failed) {
        LogInfo("host audio: waveOutWrite failed (%u), dropping PCM", (unsigned)rc);
        a->write_failed = 1;
      }
      break;
    }
    done += n;
  }
  return done;
}

void HostAudioClose(HostAudio* a) {
  if (!a) return;
  if (a->handle) {
    waveOutReset(a->handle);
    for (int i = 0; i < kHostAudioBuffers; i++) {
      if (a->header[i].dwFlags & WHDR_PREPARED)
        waveOutUnprepareHeader(a->handle, &a->header[i], sizeof(WAVEHDR));
    }
    waveOutClose(a->handle);
  }
  for (int i = 0; i < kHostAudioBuffers; i++) free(a->data[i]);
  free(a);
}
