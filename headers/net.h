#ifndef NET_H
#define NET_H
#include "arena.h"
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

/**
 * @file net.h
 * @brief Non-blocking TCP for game loops: sockets that never stall a frame,
 *        plus buffered connections that speak length-prefixed messages.
 *
 * Two layers, usable separately:
 * - **Sockets** (`net_*`): listen, accept, connect with a timeout, send and
 *   receive. Every socket handed out is non-blocking with Nagle off, so a
 *   call returns at once with "nothing yet" instead of waiting.
 * - **Connections** (::NetConn): a socket plus an input and an output buffer
 *   carved from an ::Arena. Queue bytes or whole messages, then once per
 *   frame net_conn_pump() to receive and net_conn_flush() to send. Raw bytes
 *   (an ANSI stream for a thin client) and framed messages (game events) use
 *   the same buffers.
 *
 * Typical use, a host that echoes every message back:
 * @code
 *     net_init();
 *     Arena a = arena_create(net_conn_bytes(4096, 65536) * 8 + 1024);
 *     NetSocket ls = net_listen(NULL, "7777", 8);
 *     NetConn *peers[8];
 *     int np = 0;
 *
 *     for (int running = 1; running; ) {
 *         NetSocket s = net_accept(ls);
 *         if (s != NET_INVALID && np < 8)
 *             peers[np++] = net_conn_create(&a, s, 4096, 65536);
 *
 *         for (int i = 0; i < np; i++) {
 *             NetConn *c = peers[i];
 *             net_conn_pump(c);
 *             NetMsg m;
 *             while (net_conn_recv_msg(c, &m) == 1)
 *                 net_conn_send_msg(c, m.type, m.data, m.len);
 *             net_conn_flush(c);
 *         }
 *         frame_wait(&fc);   // or net_wait() when there is no frame clock
 *     }
 *     net_close(ls);
 *     net_shutdown();
 * @endcode
 *
 * A client is the same loop around one connection:
 * @code
 *     NetSocket s = net_connect("192.168.1.20", "7777", 3000);
 *     if (s == NET_INVALID) { ... refused, unreachable or timed out ... }
 *     NetConn *c = net_conn_create(&a, s, 65536, 4096);
 * @endcode
 *
 * **Message format.** A 3-byte header, then the payload:
 * @verbatim
 *   [len hi][len lo][type][payload: len bytes]
 * @endverbatim
 * The length is big-endian and counts the payload only, so a payload holds at
 * most ::NET_MSG_MAX bytes. The type byte is yours to define. Use the
 * net_put_* / net_get_* helpers to write fields in a fixed byte order, so a
 * Windows peer and a Linux peer read the same numbers.
 *
 * @warning On Windows, link with `-lws2_32`. MSVC picks that up from the
 *          `#pragma comment` below; mingw-w64 needs it on the command line.
 * @note POSIX (BSD sockets) and Windows (Winsock 2). IPv4 and IPv6: a
 *       listener prefers one dual-stack IPv6 socket, so it also accepts IPv4
 *       peers, and falls back to IPv4 where IPv6 is disabled.
 * @note Sends never raise SIGPIPE (MSG_NOSIGNAL on Linux, SO_NOSIGPIPE on
 *       macOS/BSD), so the process's signal handling is left alone. A peer
 *       that vanishes shows up as a -1 return instead.
 * @note No threads and no global state other than the Winsock start-up count.
 */

#ifdef _MSC_VER
#pragma comment(lib, "ws2_32")
#endif

#ifdef _WIN32
typedef SOCKET NetSocket;
/** @brief The "no socket" value: what a failed or empty call returns. */
#define NET_INVALID INVALID_SOCKET
#else
typedef int NetSocket;
#define NET_INVALID (-1)
#endif

#if defined(MSG_NOSIGNAL)
#define NET_SEND_FLAGS MSG_NOSIGNAL
#else
#define NET_SEND_FLAGS 0
#endif

/** @brief Bytes in a message header: a 16-bit length and an 8-bit type. */
#define NET_MSG_HEADER 3
/** @brief Largest payload a message can carry, in bytes. */
#define NET_MSG_MAX 65535
/** @brief Most sockets net_wait() watches in one call. */
#define NET_WAIT_MAX 64

// ---------------------------------------------------------------------------
// Start-up and errors
// ---------------------------------------------------------------------------

/** @brief Whether net_init() has run. Winsock refuses every call before
 *         WSAStartup(), so this is checked rather than assumed. */
static int net_ready = 0;

/**
 * @brief Start the network layer. Call once before any other `net_*` call.
 *
 * @return 0 on success, -1 if Winsock could not start.
 *
 * @note A no-op on POSIX, and safe to call twice everywhere.
 */
static inline int net_init(void) {
  if (net_ready)
    return 0;
#ifdef _WIN32
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    return -1;
#endif
  net_ready = 1;
  return 0;
}

/**
 * @brief Shut the network layer down. Close your sockets first.
 */
static inline void net_shutdown(void) {
  if (!net_ready)
    return;
#ifdef _WIN32
  WSACleanup();
#endif
  net_ready = 0;
}

/**
 * @brief The last socket error code: errno on POSIX, WSAGetLastError() on
 *        Windows. Only meaningful straight after a call that failed.
 */
static inline int net_last_error(void) {
#ifdef _WIN32
  return WSAGetLastError();
#else
  return errno;
#endif
}

/**
 * @brief Whether the last failed send/recv/accept only means "try again
 *        later" rather than a real error.
 */
static inline int net_would_block(void) {
#ifdef _WIN32
  int e = WSAGetLastError();
  return e == WSAEWOULDBLOCK || e == WSAEINTR;
#else
  return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

// ---------------------------------------------------------------------------
// Sockets
// ---------------------------------------------------------------------------

/**
 * @brief Close a socket. NET_INVALID is ignored, so this is safe on any
 *        value a `net_*` call returned.
 */
static inline void net_close(NetSocket s) {
  if (s == NET_INVALID)
    return;
#ifdef _WIN32
  closesocket(s);
#else
  close(s);
#endif
}

/**
 * @brief Put a socket in non-blocking mode.
 *
 * @return 0 on success, -1 on error.
 */
static inline int net_set_nonblocking(NetSocket s) {
#ifdef _WIN32
  u_long on = 1;
  return ioctlsocket(s, FIONBIO, &on) == 0 ? 0 : -1;
#else
  int flags = fcntl(s, F_GETFL, 0);
  if (flags < 0)
    return -1;
  return fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0 ? 0 : -1;
#endif
}

/**
 * @brief The settings every connected socket from this header gets:
 *        non-blocking, Nagle off, and no SIGPIPE where the OS needs a socket
 *        option for that.
 *
 * @return 0 on success, -1 if the socket could not be made non-blocking.
 *         Failing to set the other options is not an error.
 *
 * @note Nagle's algorithm holds small writes back for up to ~40 ms to batch
 *       them. That is fine for file transfer and terrible for key presses.
 */
static inline int net_configure(NetSocket s) {
  int one = 1;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
#ifdef SO_NOSIGPIPE
  setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, (const char *)&one, sizeof one);
#endif
  return net_set_nonblocking(s);
}

/**
 * @brief Open a listening TCP socket.
 *
 * @param host    Local address to bind, or NULL for every interface.
 *                "127.0.0.1" or "::1" accepts only this machine.
 * @param port    Port number or service name, as a string ("7777").
 * @param backlog How many not-yet-accepted connections the OS may queue.
 * @return A non-blocking listening socket, or NET_INVALID on error (most
 *         often the port is already in use).
 *
 * @note With host NULL an IPv6 dual-stack socket is tried first, so one
 *       listener serves IPv4 and IPv6 peers. IPv4 peers then report addresses
 *       like "::ffff:192.168.1.5" from net_peer_name().
 * @note On POSIX SO_REUSEADDR is set, so a restarted host can rebind at once
 *       instead of waiting out TIME_WAIT. On Windows that option would let
 *       another program steal the port, so SO_EXCLUSIVEADDRUSE is set instead.
 */
static inline NetSocket net_listen(const char *host, const char *port,
                                   int backlog) {
  struct addrinfo hints, *res = NULL;
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  if (getaddrinfo(host, port, &hints, &res) != 0)
    return NET_INVALID;

  NetSocket s = NET_INVALID;
  for (int pass = 0; pass < 2 && s == NET_INVALID; pass++) {
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
      if ((ai->ai_family == AF_INET6) != (pass == 0))
        continue;
      s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
      if (s == NET_INVALID)
        continue;
      int one = 1, zero = 0;
#ifdef _WIN32
      setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&one, sizeof one);
#else
      setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
#endif
      if (ai->ai_family == AF_INET6)
        setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&zero, sizeof zero);
      if (bind(s, ai->ai_addr, (int)ai->ai_addrlen) == 0 &&
          listen(s, backlog) == 0 && net_set_nonblocking(s) == 0)
        break;
      net_close(s);
      s = NET_INVALID;
    }
  }
  freeaddrinfo(res);
  return s;
}

/**
 * @brief Take one waiting connection off a listener, if there is one.
 *
 * @param listener A socket from net_listen().
 * @return A configured (non-blocking, no-delay) socket for the new peer, or
 *         NET_INVALID if nobody is waiting. Call it every frame; to take a
 *         burst of connections, call it until it returns NET_INVALID.
 */
static inline NetSocket net_accept(NetSocket listener) {
  NetSocket s = accept(listener, NULL, NULL);
  if (s == NET_INVALID)
    return NET_INVALID;
  if (net_configure(s) != 0) {
    net_close(s);
    return NET_INVALID;
  }
  return s;
}

/**
 * @brief Wait until a connect in progress finishes or the time runs out.
 *
 * @return 1 when it can be written to (connected, or failed: check
 *         SO_ERROR), 0 on timeout, -1 on error.
 *
 * @note select() on Windows because WSAPoll() does not report a failed
 *       connect on older Windows 10 builds; poll() on POSIX because select()
 *       breaks for descriptors above FD_SETSIZE.
 */
static inline int net_wait_writable(NetSocket s, int timeout_ms) {
#ifdef _WIN32
  fd_set wr, ex;
  FD_ZERO(&wr);
  FD_ZERO(&ex);
  FD_SET(s, &wr);
  FD_SET(s, &ex);
  struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
  int n = select(0, NULL, &wr, &ex, timeout_ms < 0 ? NULL : &tv);
  return n < 0 ? -1 : n > 0;
#else
  struct pollfd p = {s, POLLOUT, 0};
  int n;
  do
    n = poll(&p, 1, timeout_ms);
  while (n < 0 && errno == EINTR);
  return n < 0 ? -1 : n > 0;
#endif
}

/**
 * @brief Connect to a host, giving up after a time limit.
 *
 * @param host       Name or address ("localhost", "10.0.0.4", "::1").
 * @param port       Port number or service name, as a string.
 * @param timeout_ms Limit for each address the name resolves to. A name with
 *                   both an IPv6 and an IPv4 address can take up to twice
 *                   this. -1 waits as long as the OS does.
 * @return A configured, connected socket, or NET_INVALID if every address
 *         was refused, unreachable or timed out.
 *
 * @warning Blocks for up to @p timeout_ms, and the name lookup
 *          (getaddrinfo) can block too. Call it from a "Connecting..."
 *          screen, not in the middle of a game frame.
 */
static inline NetSocket net_connect(const char *host, const char *port,
                                    int timeout_ms) {
  struct addrinfo hints, *res = NULL;
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host, port, &hints, &res) != 0)
    return NET_INVALID;

  NetSocket s = NET_INVALID;
  for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
    s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (s == NET_INVALID)
      continue;
    if (net_configure(s) == 0) {
      if (connect(s, ai->ai_addr, (int)ai->ai_addrlen) == 0)
        break;
#ifdef _WIN32
      int pending = WSAGetLastError() == WSAEWOULDBLOCK;
#else
      int pending = errno == EINPROGRESS;
#endif
      int err = 1;
      socklen_t len = sizeof err;
      if (pending && net_wait_writable(s, timeout_ms) == 1 &&
          getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &len) == 0 && err == 0)
        break;
    }
    net_close(s);
    s = NET_INVALID;
  }
  freeaddrinfo(res);
  return s;
}

/**
 * @brief Send as many bytes as the OS will take right now.
 *
 * @return Bytes sent (possibly fewer than @p len, possibly 0 when the send
 *         buffer is full), or -1 if the connection is dead.
 *
 * @note A short send is normal on a non-blocking socket. Keep the rest and
 *       retry later, which is exactly what ::NetConn does for you.
 */
static inline int net_send(NetSocket s, const void *data, size_t len) {
  if (len > INT_MAX)
    len = INT_MAX;
#ifdef _WIN32
  int n = send(s, (const char *)data, (int)len, 0);
#else
  ssize_t n = send(s, data, len, NET_SEND_FLAGS);
#endif
  if (n >= 0)
    return (int)n;
  return net_would_block() ? 0 : -1;
}

/**
 * @brief Receive whatever bytes have arrived, without waiting.
 *
 * @return Bytes read (> 0), 0 if nothing has arrived yet, or -1 if the peer
 *         closed the connection or it failed.
 *
 * @note Plain recv() returns 0 for "the peer closed". Here 0 means "nothing
 *       yet" and a close is -1, because a non-blocking loop sees "nothing yet"
 *       far more often and should not have to special-case it.
 */
static inline int net_recv(NetSocket s, void *buf, size_t cap) {
  if (cap > INT_MAX)
    cap = INT_MAX;
#ifdef _WIN32
  int n = recv(s, (char *)buf, (int)cap, 0);
#else
  ssize_t n = recv(s, buf, cap, 0);
#endif
  if (n > 0)
    return (int)n;
  if (n == 0)
    return -1;
  return net_would_block() ? 0 : -1;
}

/**
 * @brief Sleep until any of the sockets has data (or a connection waiting),
 *        or until the time runs out.
 *
 * @param socks      Sockets to watch. NET_INVALID entries are skipped.
 * @param n          How many; at most ::NET_WAIT_MAX are watched.
 * @param timeout_ms Longest wait. 0 just checks, -1 waits forever.
 * @return How many sockets are ready, 0 on timeout, -1 on error.
 *
 * @note It does not say *which* sockets are ready: pump them all. This is for
 *       a dedicated server loop with no frame clock. A game loop paced by
 *       frame_wait() does not need it.
 */
static inline int net_wait(const NetSocket *socks, int n, int timeout_ms) {
  if (n > NET_WAIT_MAX)
    n = NET_WAIT_MAX;
#ifdef _WIN32
  WSAPOLLFD p[NET_WAIT_MAX];
#else
  struct pollfd p[NET_WAIT_MAX];
#endif
  int m = 0;
  for (int i = 0; i < n; i++) {
    if (socks[i] == NET_INVALID)
      continue;
    p[m].fd = socks[i];
    p[m].events = POLLIN;
    p[m].revents = 0;
    m++;
  }
#ifdef _WIN32
  if (m == 0) {
    Sleep(timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms);
    return 0;
  }
  return WSAPoll(p, (ULONG)m, timeout_ms);
#else
  int r;
  do
    r = poll(p, (nfds_t)m, timeout_ms);
  while (r < 0 && errno == EINTR);
  return r;
#endif
}

/**
 * @brief Write the peer's address as "ip:port", or "[ip]:port" for IPv6.
 *
 * @return 0 on success, -1 on error (with @p buf set to "?").
 */
static inline int net_peer_name(NetSocket s, char *buf, size_t cap) {
  struct sockaddr_storage ss;
  socklen_t len = sizeof ss;
  char host[NI_MAXHOST], serv[NI_MAXSERV];
  if (cap)
    snprintf(buf, cap, "?");
  if (getpeername(s, (struct sockaddr *)&ss, &len) != 0 ||
      getnameinfo((struct sockaddr *)&ss, len, host, sizeof host, serv,
                  sizeof serv, NI_NUMERICHOST | NI_NUMERICSERV) != 0)
    return -1;
  snprintf(buf, cap, ss.ss_family == AF_INET6 ? "[%s]:%s" : "%s:%s", host, serv);
  return 0;
}

// ---------------------------------------------------------------------------
// Byte order
// ---------------------------------------------------------------------------

/** @brief Store @p v big-endian in 2 bytes at @p p. */
static inline void net_put_u16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}

/** @brief Store @p v big-endian in 4 bytes at @p p. */
static inline void net_put_u32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

/** @brief Read a big-endian 16-bit value from @p p. */
static inline uint16_t net_get_u16(const uint8_t *p) {
  return (uint16_t)((p[0] << 8) | p[1]);
}

/** @brief Read a big-endian 32-bit value from @p p. */
static inline uint32_t net_get_u32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | p[3];
}

// ---------------------------------------------------------------------------
// Buffered connections
// ---------------------------------------------------------------------------

/**
 * @brief A socket with its own receive and send buffers.
 *
 * Unread input lives in `in[in_start, in_start + in_len)`; queued output in
 * `out[0, out_len)`. Both buffers come from the ::Arena passed to
 * net_conn_create() and keep their size for the connection's life.
 */
typedef struct NetConn {
  NetSocket sock;
  uint8_t *in, *out;
  size_t in_cap, out_cap;
  size_t in_start, in_len;
  size_t out_len;
  int dead; // set once the peer closes or a send/recv fails
} NetConn;

/** @brief One received message. `data` points into the connection's input
 *         buffer and stays valid until the next net_conn_pump(). */
typedef struct NetMsg {
  uint8_t type;
  uint16_t len;
  const uint8_t *data;
} NetMsg;

/**
 * @brief Arena bytes one net_conn_create() call needs, alignment included.
 */
static inline size_t net_conn_bytes(size_t in_cap, size_t out_cap) {
  return arena_align_up(sizeof(NetConn), ARENA_DEFAULT_ALIGN) +
         arena_align_up(in_cap, ARENA_DEFAULT_ALIGN) +
         arena_align_up(out_cap, ARENA_DEFAULT_ALIGN);
}

/**
 * @brief Point an existing connection at a new socket, emptying its buffers.
 *
 * Arena memory is never freed one piece at a time, so when a peer leaves,
 * reuse its ::NetConn for the next one instead of creating another.
 */
static inline void net_conn_attach(NetConn *c, NetSocket s) {
  c->sock = s;
  c->in_start = c->in_len = c->out_len = 0;
  c->dead = s == NET_INVALID;
}

/**
 * @brief Allocate a connection and its buffers from an arena.
 *
 * @param a       Arena to carve from; needs net_conn_bytes() free.
 * @param s       A connected socket (net_accept() / net_connect()). The
 *                connection owns it from here on.
 * @param in_cap  Receive buffer size. Must hold the largest message you
 *                expect plus ::NET_MSG_HEADER, or that message is never read.
 * @param out_cap Send buffer size: how far ahead of the network you can
 *                queue before net_conn_write() starts refusing.
 * @return The connection, or NULL if the arena is too small (the socket is
 *         left open in that case).
 */
static inline NetConn *net_conn_create(Arena *a, NetSocket s, size_t in_cap,
                                       size_t out_cap) {
  NetConn *c = (NetConn *)arena_alloc(a, sizeof(NetConn));
  uint8_t *in = (uint8_t *)arena_alloc(a, in_cap);
  uint8_t *out = (uint8_t *)arena_alloc(a, out_cap);
  if (!c || !in || !out)
    return NULL;
  c->in = in;
  c->out = out;
  c->in_cap = in_cap;
  c->out_cap = out_cap;
  net_conn_attach(c, s);
  return c;
}

/**
 * @brief Close the socket and mark the connection dead. The buffers stay
 *        allocated for net_conn_attach() to reuse.
 */
static inline void net_conn_close(NetConn *c) {
  net_close(c->sock);
  c->sock = NET_INVALID;
  c->dead = 1;
}

/**
 * @brief Read everything that has arrived into the input buffer.
 *
 * @return Bytes received this call, or -1 if the connection is dead.
 *
 * @note Invalidates the `data` pointers of messages read earlier, because
 *       unread input is moved to the front of the buffer first.
 * @note A dead connection's last complete messages can still be read with
 *       net_conn_recv_msg(), so a "goodbye" sent just before closing arrives.
 */
static inline int net_conn_pump(NetConn *c) {
  if (c->in_start) {
    memmove(c->in, c->in + c->in_start, c->in_len);
    c->in_start = 0;
  }
  if (c->dead)
    return -1;
  int total = 0;
  while (c->in_len < c->in_cap) {
    int n = net_recv(c->sock, c->in + c->in_len, c->in_cap - c->in_len);
    if (n < 0) {
      net_conn_close(c);
      return -1;
    }
    if (n == 0)
      break;
    c->in_len += (size_t)n;
    total += n;
  }
  return total;
}

/**
 * @brief Send as much queued output as the OS will take.
 *
 * @return Bytes still queued afterwards (0 means all sent), or -1 if the
 *         connection is dead.
 */
static inline long net_conn_flush(NetConn *c) {
  if (c->dead)
    return -1;
  size_t sent = 0;
  while (sent < c->out_len) {
    int n = net_send(c->sock, c->out + sent, c->out_len - sent);
    if (n < 0) {
      net_conn_close(c);
      return -1;
    }
    if (n == 0)
      break;
    sent += (size_t)n;
  }
  memmove(c->out, c->out + sent, c->out_len - sent);
  c->out_len -= sent;
  return (long)c->out_len;
}

/**
 * @brief Make room for @p need more output bytes, flushing if necessary.
 *
 * @return 0 if they fit, -1 if the connection is dead or still too full.
 */
static inline int net_conn_reserve(NetConn *c, size_t need) {
  if (c->dead)
    return -1;
  if (c->out_len + need > c->out_cap && net_conn_flush(c) < 0)
    return -1;
  return c->out_len + need <= c->out_cap ? 0 : -1;
}

/**
 * @brief Queue raw bytes for sending. Nothing goes out until
 *        net_conn_flush().
 *
 * @return 0 if queued, -1 if the connection is dead or the bytes don't fit
 *         even after a flush (a slow peer has fallen too far behind). Nothing
 *         is queued on failure.
 */
static inline int net_conn_write(NetConn *c, const void *data, size_t len) {
  if (net_conn_reserve(c, len) != 0)
    return -1;
  memcpy(c->out + c->out_len, data, len);
  c->out_len += len;
  return 0;
}

/**
 * @brief Queue one framed message: header, then payload.
 *
 * @param type Message kind, defined by the program.
 * @param data Payload; may be NULL when @p len is 0.
 * @param len  Payload size, at most ::NET_MSG_MAX.
 * @return 0 if queued, -1 if too long, dead, or no room. The message is
 *         queued whole or not at all, so the stream never holds half of one.
 */
static inline int net_conn_send_msg(NetConn *c, uint8_t type, const void *data,
                                    size_t len) {
  if (len > NET_MSG_MAX || net_conn_reserve(c, NET_MSG_HEADER + len) != 0)
    return -1;
  uint8_t *p = c->out + c->out_len;
  net_put_u16(p, (uint16_t)len);
  p[2] = type;
  if (len)
    memcpy(p + NET_MSG_HEADER, data, len);
  c->out_len += NET_MSG_HEADER + len;
  return 0;
}

/**
 * @brief Look at the unread input without taking it, for raw streams.
 *
 * @param len Out-parameter for the number of unread bytes.
 * @return Pointer to them, valid until the next net_conn_pump().
 */
static inline const uint8_t *net_conn_peek(const NetConn *c, size_t *len) {
  *len = c->in_len;
  return c->in + c->in_start;
}

/** @brief Drop @p n bytes from the front of the unread input. */
static inline void net_conn_consume(NetConn *c, size_t n) {
  if (n > c->in_len)
    n = c->in_len;
  c->in_start += n;
  c->in_len -= n;
}

/**
 * @brief Take the next complete message from the input, if one is there.
 *
 * @param m Filled in on success. Its `data` points into the input buffer.
 * @return 1 for a message, 0 if the next one has not fully arrived, -1 if the
 *         next one is bigger than the input buffer and can never be read (the
 *         connection is closed, since the stream can't be resynchronised).
 *
 * @note Call in a loop until it stops returning 1: one pump can bring in
 *       many messages.
 */
static inline int net_conn_recv_msg(NetConn *c, NetMsg *m) {
  if (c->in_len < NET_MSG_HEADER)
    return 0;
  const uint8_t *p = c->in + c->in_start;
  size_t len = net_get_u16(p);
  if (NET_MSG_HEADER + len > c->in_cap) {
    net_conn_close(c);
    return -1;
  }
  if (c->in_len < NET_MSG_HEADER + len)
    return 0;
  m->type = p[2];
  m->len = (uint16_t)len;
  m->data = p + NET_MSG_HEADER;
  net_conn_consume(c, NET_MSG_HEADER + len);
  return 1;
}

#endif
