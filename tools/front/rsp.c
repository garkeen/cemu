// RSP client wire code (tools/front; the host-side sibling of the stub's
// debug/gdbstub.c framing — same packet shape, client role).
#include "rsp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host/host.h"

enum { kPktMax = 4096, kBufMax = 16384 };

struct Rsp {
  HostSock* sock;
  uint8_t buf[kBufMax];  // bytes received but not yet consumed by the parser
  int len;
};

Rsp* RspConnect(const char* hostport) {
  // "host:port" — split at the last colon (IPv6 literals are not a use case
  // here; the stub binds 0.0.0.0).
  const char* colon = strrchr(hostport, ':');
  if (!colon || !colon[1]) return NULL;
  char host[128];
  size_t n = (size_t)(colon - hostport);
  if (n >= sizeof(host)) return NULL;
  memcpy(host, hostport, n);
  host[n] = 0;
  int port = atoi(colon + 1);
  if (port <= 0 || port > 65535) return NULL;
  HostSock* sock = HostSockConnect(host, port);
  if (!sock) return NULL;
  Rsp* r = (Rsp*)calloc(1, sizeof(*r));
  if (!r) {
    HostSockClose(sock);
    return NULL;
  }
  r->sock = sock;
  return r;
}

void RspClose(Rsp* r) {
  if (!r) return;
  HostSockClose(r->sock);
  free(r);
}

int RspHexVal(uint8_t c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Appends whatever the socket has ready (non-blocking); -1 = connection lost.
static int Fill(Rsp* r) {
  uint8_t tmp[512];
  while (HostSockReadable(r->sock)) {
    int n = HostSockRead(r->sock, tmp, (int)sizeof(tmp));
    if (n <= 0) return -1;
    if (r->len + n > (int)sizeof(r->buf)) n = (int)sizeof(r->buf) - r->len;
    if (n <= 0) return 0;  // buffer full: parser must consume first
    memcpy(r->buf + r->len, tmp, (size_t)n);
    r->len += n;
  }
  return 0;
}

// Blocking variant of Fill: reads at least one byte; 0 = ok, -1 = lost.
static int FillBlock(Rsp* r) {
  uint8_t tmp[512];
  int n = HostSockRead(r->sock, tmp, (int)sizeof(tmp));
  if (n <= 0) return -1;
  if (r->len + n > (int)sizeof(r->buf)) n = (int)sizeof(r->buf) - r->len;
  memcpy(r->buf + r->len, tmp, (size_t)n);
  r->len += n;
  return 0;
}

// Consumes one packet from the buffer if one is complete; 1 = packet, 0 =
// need more bytes. Out-of-packet bytes are acks ('+'/'-') and noise: dropped.
static int Parse(Rsp* r, char* pkt, int cap) {
  for (int i = 0; i < r->len; i++) {
    uint8_t b = r->buf[i];
    if (b != '$') continue;  // acks and stray bytes between packets
    uint8_t sum = 0;
    int len = 0;
    int j = i + 1;
    for (; j < r->len && r->buf[j] != '#'; j++) {
      uint8_t c = r->buf[j];
      if (c == '}' && j + 1 < r->len) {  // binary escape: next byte XOR 0x20
        c = (uint8_t)(r->buf[j + 1] ^ 0x20);
        j++;
      }
      sum = (uint8_t)(sum + c);
      if (len < cap - 1) pkt[len++] = (char)c;
    }
    if (j + 2 >= r->len) return 0;  // trailer not here yet
    int hi = RspHexVal(r->buf[j + 1]), lo = RspHexVal(r->buf[j + 2]);
    if (hi < 0 || lo < 0 || ((hi << 4) | lo) != sum) {
      // Bad checksum: drop through the '$' and resync (the stub resends
      // only on '-', which it never sees from us — local TCP just loses).
      i = j + 2;
      continue;
    }
    pkt[len] = 0;
    int consumed = j + 3;
    memmove(r->buf, r->buf + consumed, (size_t)(r->len - consumed));
    r->len -= consumed;
    uint8_t ack = '+';
    HostSockWrite(r->sock, &ack, 1);
    return 1;
  }
  return 0;
}

int RspSend(Rsp* r, const char* pkt) {
  size_t dlen = strlen(pkt);
  if (dlen > kPktMax - 4) return -1;
  uint8_t frame[kPktMax + 4];
  uint8_t sum = 0;
  for (size_t i = 0; i < dlen; i++) sum = (uint8_t)(sum + (uint8_t)pkt[i]);
  int n = 0;
  frame[n++] = '$';
  memcpy(frame + n, pkt, dlen);
  n += (int)dlen;
  frame[n++] = '#';
  frame[n++] = (uint8_t)"0123456789abcdef"[sum >> 4];
  frame[n++] = (uint8_t)"0123456789abcdef"[sum & 15];
  if (!HostSockWrite(r->sock, frame, n)) return -1;
  uint8_t ack;
  if (!HostSockRead(r->sock, &ack, 1)) return -1;  // blocking: '+' (or noise)
  return 0;
}

int RspInterrupt(Rsp* r) {
  uint8_t b = 0x03;
  return HostSockWrite(r->sock, &b, 1) ? 0 : -1;
}

int RspRecv(Rsp* r, char* pkt, int cap) {
  for (;;) {
    if (Parse(r, pkt, cap)) return (int)strlen(pkt);
    if (FillBlock(r)) return -1;
  }
}

int RspPoll(Rsp* r, char* pkt, int cap) {
  if (Fill(r)) return -1;
  return Parse(r, pkt, cap) ? 1 : 0;
}

// ---- target description -------------------------------------------------------

// Copies attribute VALUE from a "<tag ... name="..." ...>" element prefix.
// The search is bounded to THIS element (up to '>'): an attribute a register
// lacks must not leak from a later element (every reg would otherwise
// inherit fctrl's group="float").
static int Attr(const char* p, const char* name, char* out, int cap) {
  const char* e = strchr(p, '>');
  if (!e) return 0;
  char pat[32];
  snprintf(pat, sizeof(pat), "%s=\"", name);
  const char* a = strstr(p, pat);
  if (!a || a >= e) return 0;
  a += strlen(pat);
  const char* q = strchr(a, '"');
  if (!q || q > e) return 0;
  size_t n = (size_t)(q - a);
  if (n >= (size_t)cap) n = (size_t)cap - 1;
  memcpy(out, a, n);
  out[n] = 0;
  return 1;
}

int RspReadRegTable(Rsp* r, RspRegTable* t) {
  static char xml[32768];
  int xlen = 0;
  uint64_t off = 0;
  memset(t, 0, sizeof(*t));
  for (;;) {
    char cmd[96], pkt[kPktMax];
    snprintf(cmd, sizeof(cmd), "qXfer:features:read:target.xml:%llx,fff",
             (unsigned long long)off);
    if (RspSend(r, cmd)) return -1;
    if (RspRecv(r, pkt, sizeof(pkt)) <= 0) return -1;
    if (pkt[0] != 'm' && pkt[0] != 'l') return -1;
    size_t chunk = strlen(pkt + 1);
    if (xlen + (int)chunk >= (int)sizeof(xml)) return -1;
    memcpy(xml + xlen, pkt + 1, chunk);
    xlen += (int)chunk;
    off += chunk;
    if (pkt[0] == 'l') break;
    if (chunk == 0) return -1;
  }
  xml[xlen] = 0;
  const char* p = xml;
  while ((p = strstr(p, "<reg ")) != NULL && t->n < kRspMaxRegs) {
    RspReg* reg = &t->reg[t->n];
    char bits[16], group[32] = "", type[32] = "";
    if (Attr(p, "name", reg->name, kRspNameMax) && Attr(p, "bitsize", bits, sizeof(bits))) {
      reg->size = atoi(bits) / 8;
      Attr(p, "group", group, sizeof(group));
      Attr(p, "type", type, sizeof(type));
      reg->hidden = strcmp(group, "float") == 0 || strncmp(type, "i387", 4) == 0;
      reg->code_ptr = strcmp(type, "code_ptr") == 0;
      if (reg->size > 0) t->n++;
    }
    p++;
  }
  return t->n > 0 ? 0 : -1;
}
