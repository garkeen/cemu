// Winsock transport for the gdb stub (stage 3.5). The only socket code in
// the tree lives here — the stub above it (debug/) sees HostSock handles.

#include <winsock2.h>
#include <ws2tcpip.h>

#include <stdio.h>
#include <stdlib.h>

#include "host/host.h"
#include "util/log.h"

typedef struct HostSock {
  SOCKET s;
} HostSock;

static int wsa_ready;

static int WsaInit(void) {
  if (wsa_ready) return 0;
  WSADATA d;
  if (WSAStartup(MAKEWORD(2, 2), &d) != 0) return -1;
  wsa_ready = 1;
  return 0;
}

HostSock* HostSockListen(int port) {
  if (WsaInit()) return NULL;
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) return NULL;
  BOOL reuse = TRUE;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_ANY);  // QEMU `-gdb tcp::port` binds all
  a.sin_port = htons((u_short)port);
  if (bind(s, (struct sockaddr*)&a, sizeof(a)) != 0 || listen(s, 1) != 0) {
    closesocket(s);
    return NULL;
  }
  HostSock* h = (HostSock*)calloc(1, sizeof(HostSock));
  if (!h) {
    closesocket(s);
    return NULL;
  }
  h->s = s;
  return h;
}

static HostSock* AcceptFrom(SOCKET l) {
  SOCKET s = accept(l, NULL, NULL);
  if (s == INVALID_SOCKET) return NULL;
  HostSock* h = (HostSock*)calloc(1, sizeof(HostSock));
  if (!h) {
    closesocket(s);
    return NULL;
  }
  h->s = s;
  return h;
}

HostSock* HostSockConnect(const char* host, int port) {
  if (WsaInit()) return NULL;
  char port_str[16];
  snprintf(port_str, sizeof(port_str), "%d", port);
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* ai = NULL;
  if (getaddrinfo(host, port_str, &hints, &ai) != 0 || !ai) return NULL;
  SOCKET s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
  if (s == INVALID_SOCKET) {
    freeaddrinfo(ai);
    return NULL;
  }
  if (connect(s, ai->ai_addr, (int)ai->ai_addrlen) != 0) {
    freeaddrinfo(ai);
    closesocket(s);
    return NULL;
  }
  freeaddrinfo(ai);
  // The cemugui front-end runs its protocol exchanges on the UI thread: a
  // silent peer must degrade to a failed request (and a disconnect notice),
  // never an unbounded block. 3s is far above any local round trip.
  DWORD rcv_timeout_ms = 3000;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rcv_timeout_ms, sizeof(rcv_timeout_ms));
  HostSock* h = (HostSock*)calloc(1, sizeof(HostSock));
  if (!h) {
    closesocket(s);
    return NULL;
  }
  h->s = s;
  return h;
}

HostSock* HostSockAccept(HostSock* l) {
  return l ? AcceptFrom(l->s) : NULL;
}

HostSock* HostSockAcceptPoll(HostSock* l) {
  if (!l) return NULL;
  fd_set r;
  FD_ZERO(&r);
  FD_SET(l->s, &r);
  struct timeval zero = {0, 0};
  if (select((int)l->s + 1, &r, NULL, NULL, &zero) <= 0) return NULL;
  return AcceptFrom(l->s);
}

int HostSockRead(HostSock* s, void* buf, int cap) {
  if (!s || cap <= 0) return 0;
  int n = recv(s->s, (char*)buf, cap, 0);
  return n > 0 ? n : 0;  // any error/EOF reads as closed
}

int HostSockWrite(HostSock* s, const void* buf, int n) {
  if (!s) return 0;
  const char* p = (const char*)buf;
  int left = n;
  while (left > 0) {
    int k = send(s->s, p, left, 0);
    if (k <= 0) return 0;
    p += k;
    left -= k;
  }
  return n;
}

int HostSockReadable(HostSock* s) {
  if (!s) return 0;
  fd_set r;
  FD_ZERO(&r);
  FD_SET(s->s, &r);
  struct timeval zero = {0, 0};
  return select((int)s->s + 1, &r, NULL, NULL, &zero) > 0;
}

void HostSockClose(HostSock* s) {
  if (!s) return;
  closesocket(s->s);
  free(s);
}
