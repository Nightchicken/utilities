#ifndef AIDS
#define AIDS

/*
 *INFO:
 * Standard Terminal Interface
 * This is a collection of headers which I normally use when working in the terminal for creating games or apps.
 * Arena, Input handling, Display helpers, and Sockets.
 *
 */
// NOTE: Arena headers
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
// NOTE: Socket Headers
#include <limits.h>
#include <stdio.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
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
// NOTE: Display
#include <time.h>
#ifdef _WIN32
#undef RGB // wingdi.h's; this header defines its own below
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#ifndef DISABLE_NEWLINE_AUTO_RETURN
#define DISABLE_NEWLINE_AUTO_RETURN 0x0008
#endif
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#else
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

// Arena Start
typedef struct{
	size_t size; 
	size_t capacity; // total size of `data`, or 0 if the malloc failed
	unsigned char* data;
}Arena;

#define ARENA_DEFAULT_ALIGN (sizeof(void*) > 8 ? sizeof(void*) : 8)

Arena arena_create(size_t capacity){
	Arena a;
	a.size = 0;
	a.capacity = capacity;
	a.data = (unsigned char*)malloc(capacity);
	if (!a.data) a.capacity = 0;
	return a;
}

size_t arena_align_up(size_t n, size_t align){
	return (n + (align - 1)) & ~(align - 1);
}

void* arena_alloc_aligned(Arena* self, size_t size, size_t align){
	size_t offset = arena_align_up(self->size, align);
	if (offset + size > self->capacity) return NULL;
	void* ptr = self->data + offset;
	self->size = offset + size;
	return ptr;
}

void* arena_alloc(Arena* self, size_t size){
	return arena_alloc_aligned(self, size, ARENA_DEFAULT_ALIGN);
}

void* arena_calloc(Arena* self, size_t size){
	void* ptr = arena_alloc(self, size);
	if (ptr) memset(ptr, 0, size);
	return ptr;
}
void arena_destroy(Arena* self){
	free(self->data);
	self->data = NULL;
	self->size = 0;
	self->capacity = 0;
}


void arena_reset(Arena* self){
	self->size = 0;
}
// END OF ARENA

#ifdef _MSC_VER
#pragma comment(lib, "ws2_32")
#endif

#ifdef _WIN32
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
 * @warning Close your sockets first. An open socket may lead to vulnerabilities
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
 * @param host       Name or address ("localhost", "10.0.0.4", "::1").
 * @param port       Port number or service name, as a string.
 * @param timeout_ms Limit for each address the name resolves to. A name with
 *                   both an IPv6 and an IPv4 address can take up to twice
 *                   this. -1 waits as long as the OS does.
 * @return A configured, connected socket, or NET_INVALID if every address
 *         was refused, unreachable or timed out.
 * WARNING: 
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


/**
 * @file clipboard.h
 * @brief Plain-text copy and paste with the system clipboard, for terminal
 *        programs that have no window of their own.
 *
 * @code
 *     clip_copy("hello");
 *     char buf[256];
 *     if (clip_paste(buf, sizeof buf) >= 0)
 *         printf("clipboard: %s\n", buf);
 * @endcode
 *
 * Where the text goes:
 * - **Windows**: the Win32 clipboard (CF_TEXT). Always available.
 * - **macOS**: `pbcopy` / `pbpaste`.
 * - **Linux/BSD, Wayland**: `wl-copy` / `wl-paste` (package wl-clipboard).
 * - **Linux/BSD, X11**: `xclip`, or else `xsel`.
 *
 * On POSIX, clip_copy() also sends an OSC 52 escape sequence to the terminal,
 * which sets the clipboard in terminals that support it (kitty, Alacritty,
 * WezTerm, foot, iTerm2, Windows Terminal, and over SSH). That makes copy
 * work even with no clipboard tool installed. Paste has no such fallback: it
 * needs one of the tools, or the terminal's own paste (Ctrl+Shift+V).
 * WARNING:
 * @warning The POSIX tools run through popen(), which blocks for as long as
 *          the tool takes (normally a few milliseconds). Call these on a key
 *          press, not every frame.
 *
 * @note Text is treated as bytes. Callers that want ASCII only should filter
 *       what clip_paste() returns.
 */



/** @brief Longest text clip_copy() sends through OSC 52, before base64. */
#define CLIP_OSC52_MAX 4096

#ifndef _WIN32
/** @brief Write @p text as an OSC 52 "set clipboard" sequence to stdout. */
static inline void clip_osc52(const char *text) {
  static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t n = strlen(text);
  if (n > CLIP_OSC52_MAX)
    n = CLIP_OSC52_MAX;
  char out[16 + (CLIP_OSC52_MAX + 2) / 3 * 4];
  size_t o = (size_t)snprintf(out, sizeof out, "\x1b]52;c;");
  const unsigned char *s = (const unsigned char *)text;
  for (size_t i = 0; i < n; i += 3) {
    unsigned v = (unsigned)s[i] << 16 | (i + 1 < n ? (unsigned)s[i + 1] << 8 : 0) | (i + 2 < n ? s[i + 2] : 0);
    out[o++] = b64[v >> 18 & 63];
    out[o++] = b64[v >> 12 & 63];
    out[o++] = i + 1 < n ? b64[v >> 6 & 63] : '=';
    out[o++] = i + 2 < n ? b64[v & 63] : '=';
  }
  out[o++] = '\a';
  fwrite(out, 1, o, stdout);
  fflush(stdout);
}

/**
 * @brief The shell commands to try, in order, for the current session.
 *
 * Wayland and X11 are chosen from WAYLAND_DISPLAY and DISPLAY, so a tool for
 * the wrong display server is never started. Every command discards its own
 * output so nothing lands on a full-screen terminal.
 */
static inline int clip_commands(int paste, const char **cmds) {
  int n = 0;
#ifdef __APPLE__
  cmds[n++] = paste ? "pbpaste 2>/dev/null" : "pbcopy >/dev/null 2>&1";
#else
  const char *wl = getenv("WAYLAND_DISPLAY"), *x = getenv("DISPLAY");
  if (wl && *wl)
    cmds[n++] = paste ? "wl-paste -n 2>/dev/null" : "wl-copy >/dev/null 2>&1";
  if (x && *x) {
    cmds[n++] = paste ? "xclip -o -selection clipboard 2>/dev/null"
                      : "xclip -i -selection clipboard >/dev/null 2>&1";
    cmds[n++] = paste ? "xsel -ob 2>/dev/null" : "xsel -ib >/dev/null 2>&1";
  }
#endif
  return n;
}
#endif

/**
 * @brief Put @p text on the system clipboard.
 *
 * @return 0 if a clipboard accepted it, -1 if none did. On POSIX the OSC 52
 *         sequence is sent either way, so -1 does not rule out success in a
 *         terminal that supports it.
 */
static inline int clip_copy(const char *text) {
#ifdef _WIN32
  size_t n = strlen(text) + 1;
  HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, n);
  if (!mem)
    return -1;
  memcpy(GlobalLock(mem), text, n);
  GlobalUnlock(mem);
  if (!OpenClipboard(NULL)) {
    GlobalFree(mem);
    return -1;
  }
  EmptyClipboard();
  int ok = SetClipboardData(CF_TEXT, mem) != NULL;
  CloseClipboard();
  if (!ok)
    GlobalFree(mem);
  return ok ? 0 : -1;
#else
  clip_osc52(text);
  const char *cmds[4];
  int n = clip_commands(0, cmds);
  for (int i = 0; i < n; i++) {
    FILE *p = popen(cmds[i], "w");
    if (!p)
      continue;
    fputs(text, p);
    if (pclose(p) == 0)
      return 0;
  }
  return -1;
#endif
}

/**
 * @brief Read the system clipboard into @p buf as a NUL-terminated string.
 *
 * @param buf Destination. Text longer than @p cap - 1 bytes is cut short.
 * @param cap Size of @p buf in bytes; must be at least 1.
 * @return Bytes written (not counting the NUL), or -1 if no clipboard could
 *         be read. An empty clipboard returns 0.
 */
static inline int clip_paste(char *buf, size_t cap) {
  buf[0] = 0;
#ifdef _WIN32
  if (!OpenClipboard(NULL))
    return -1;
  HANDLE h = GetClipboardData(CF_TEXT);
  const char *s = h ? (const char *)GlobalLock(h) : NULL;
  int n = -1;
  if (s) {
    n = (int)strnlen(s, cap - 1);
    memcpy(buf, s, (size_t)n);
    buf[n] = 0;
    GlobalUnlock(h);
  }
  CloseClipboard();
  return n;
#else
  const char *cmds[4];
  int n = clip_commands(1, cmds);
  for (int i = 0; i < n; i++) {
    FILE *p = popen(cmds[i], "r");
    if (!p)
      continue;
    size_t len = fread(buf, 1, cap - 1, p);
    char sink[256];
    while (fread(sink, 1, sizeof sink, p) > 0) {
    }
    buf[len] = 0;
    if (pclose(p) == 0)
      return (int)len;
    buf[0] = 0;
  }
  return -1;
#endif
}

/**
 * @file terminal.h
 * @brief Full-screen terminal renderer: raw-mode input, a frame clock, and a
 *        double-buffered character surface that diffs itself to ANSI escapes.
 *
 * Three independent pieces, usable separately:
 * - **Terminal setup** (`term_*`): raw mode, the alternate screen, size
 *   queries and non-blocking key reads.
 * - **Frame clock** (::FrameClock): absolute-deadline pacing that does not
 *   drift and absorbs render cost into the frame budget.
 * - **Screen** (::Screen): a w-by-h grid of characters with 24-bit colours,
 *   drawn into freely and then sent to the terminal as the minimal set of
 *   escape sequences, in one write() per frame.
 *
 * Typical use:
 * @code
 *     Arena a = arena_create(screen_arena_bytes(120, 40) + 1024);
 *     Screen *s = screen_create(&a, 120, 40);
 *
 *     FrameClock fc;
 *     frame_clock_init(&fc, 60);
 *     term_init();
 *
 *     char keys[32];
 *     for (int running = 1; running; ) {
 *         int n = term_read_keys(keys, sizeof keys);
 *         for (int i = 0; i < n; i++)
 *             if (keys[i] == 'q') running = 0;   // Ctrl-C will NOT quit
 *
 *         screen_clear(s, ' ', RGB(200,200,200), BG_NONE);  // every frame
 *         screen_text(s, 2, 1, "hello", RGB(255,80,80));
 *         screen_present(s);
 *
 *         frame_wait(&fc);
 *     }
 *     term_restore();
 *     arena_destroy(&a);
 * @endcode
 *
 * **ANSI control sequences.** "\x1b[" is CSI (Control Sequence Introducer):
 * the terminal reads what follows as a command instead of printing it. A
 * leading "?" parameter marks a private/DEC mode, where 'h' means set and 'l'
 * means reset -- that is the only reason ::TERM_ALT_ON and ::TERM_ALT_OFF
 * differ by one letter. Two more sequences are built by hand in
 * screen_present() rather than being macros here, because their numbers come
 * from data:
 * @verbatim
 *   "\x1b[<row>;<col>H"        move cursor (1-based, row first)
 *   "\x1b[38;2;<r>;<g>;<b>m"   set foreground to a 24-bit colour
 *   "\x1b[48;2;<r>;<g>;<b>m"   same for background ("49m" resets to default)
 * @endverbatim
 * The 38/48 codes take a colour-space selector; "2" means direct RGB, as
 * opposed to "5" which would mean a 256-colour palette index.
 *
 * @warning term_init() disables ISIG, so **Ctrl-C no longer kills the
 *          process** -- it arrives as a 0x03 byte. Your loop owns quitting,
 *          and must call term_restore() on every exit path or leave the user
 *          in a raw-mode shell with no echo.
 * @warning Defines a type named `Screen`, which collides with the unrelated
 *          `Screen` in raster.h. Only one of the two headers can be included
 *          in a given translation unit.
 * @note POSIX (termios, ioctl, clock_nanosleep) and Windows 10+ (console VT
 *       mode, QueryPerformanceCounter, a high-resolution waitable timer). On
 *       POSIX it needs `-std=gnu11` or `-D_POSIX_C_SOURCE=200809L`; strict
 *       `-std=c11` hides CLOCK_MONOTONIC and TIMER_ABSTIME. On Windows use
 *       Windows Terminal or a Windows 10+ console; the legacy console has no
 *       VT support and term_init() cannot fix that.
 * @note Cells are single bytes, so the surface is ASCII only -- a multi-byte
 *       UTF-8 glyph would span several cells and break the per-cell diff.
 */

/** @brief Switch to the alternate screen buffer (a second, scrollback-less
 *         screen), so quitting can restore the user's shell history untouched.
 *         "?" marks a private DEC mode and the trailing 'h' means set. */
#define TERM_ALT_ON "\x1b[?1049h"
/** @brief Switch back from the alternate screen buffer, restoring the user's
 *         scrollback. Same mode as ::TERM_ALT_ON with 'l' (reset) for 'h'. */
#define TERM_ALT_OFF "\x1b[?1049l"
/** @brief Hide the hardware cursor, which would otherwise flicker as the
 *         renderer moves it around the screen. */
#define TERM_HIDE_CUR "\x1b[?25l"
/** @brief Show the hardware cursor again. Pair with ::TERM_HIDE_CUR. */
#define TERM_SHOW_CUR "\x1b[?25h"
/** @brief Erase the whole screen (the "2" selects the entire display).
 *         Does not move the cursor -- follow with ::TERM_HOME if you need
 *         a known position. */
#define TERM_CLEAR "\x1b[2J"
/** @brief Move the cursor to row 1, column 1. Note ANSI positions are
 *         1-based, not 0-based. */
#define TERM_HOME "\x1b[H"
/** @brief Clear all colour and style attributes. Emit this before leaving the
 *         alternate screen, or styling leaks onto the restored shell prompt. */
#define TERM_SGR_RESET "\x1b[0m"

// ---------------------------------------------------------------------------
// Terminal setup
// ---------------------------------------------------------------------------

/** @brief The terminal settings saved by term_init(), for term_restore() to
 *         put back. File-scope, so only one terminal can be managed at a
 *         time -- which is the only sane reading of "the" terminal anyway. */
#ifdef _WIN32
static DWORD term_saved_in, term_saved_out;
#else
static struct termios term_saved;
#endif
/** @brief How long term_read_keys() waits for a key, in tenths of a second.
 *         Set with term_set_read_timeout(). */
static int term_read_timeout = 1;
/** @brief Whether ::term_saved actually holds settings worth restoring. Stays
 *         0 when stdin is not a terminal (piped input, a cron job), because
 *         restoring garbage termios onto whatever stdin really is would be
 *         worse than not restoring at all. */
static int term_saved_valid = 0;

/**
 * @brief Current terminal size in character cells, via the TIOCGWINSZ ioctl.
 *
 * @param cols Out-parameter for the width in columns. Left untouched on
 *             failure, so a caller's default survives.
 * @param rows Out-parameter for the height in rows. Same on failure.
 * @return 0 on success, -1 on error.
 *
 * @note The ws_col == 0 test matters as much as the -1 test: when stdout is a
 *       pipe or a pty with no size set, the ioctl can succeed and still hand
 *       back zeros. A caller that trusted that would compute an empty viewport
 *       and draw nothing.
 * @note Queries stdout, not stdin, so it still works with stdin redirected.
 */
static inline int term_size(int *cols, int *rows) {
#ifdef _WIN32
  // The visible window, not the buffer: the buffer can be thousands of rows.
  CONSOLE_SCREEN_BUFFER_INFO info;
  if (!GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info))
    return -1;
  int c = info.srWindow.Right - info.srWindow.Left + 1;
  int r = info.srWindow.Bottom - info.srWindow.Top + 1;
  if (c <= 0 || r <= 0)
    return -1;
  *cols = c;
  *rows = r;
  return 0;
#else
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
    return -1;
  *cols = ws.ws_col;
  *rows = ws.ws_row;
  return 0;
#endif
}

/**
 * @brief Write exactly @p len bytes to stdout, looping over short writes.
 *
 * @param s   Bytes to write. Not treated as a string -- may contain NULs.
 * @param len Number of bytes to write.
 *
 * @note write() is allowed to transfer fewer bytes than asked (a full pipe
 *       buffer, or a signal arriving mid-call), so a single call is not
 *       enough; the loop keeps pushing from where it left off.
 * @note Gives up silently on n <= 0, so a closed or broken stdout drops the
 *       frame instead of spinning forever. There is no error return.
 * @note This is write(2), not stdio: bypassing FILE* buffering is what lets
 *       screen_present() guarantee one frame reaches the terminal as one
 *       syscall.
 */
static inline void term_write_n(const char *s, size_t len) {
  size_t off = 0;
#ifdef _WIN32
  HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  while (off < len) {
    DWORD n = 0;
    DWORD want = len - off > 0x7fffffff ? 0x7fffffff : (DWORD)(len - off);
    if (!WriteFile(out, s + off, want, &n, NULL) || n == 0)
      break;
    off += n;
  }
#else
  while (off < len) {
    ssize_t n = write(STDOUT_FILENO, s + off, len - off);
    if (n <= 0)
      break;
    off += (size_t)n;
  }
#endif
}

/**
 * @brief term_write_n() for a NUL-terminated string.
 *
 * @param s String to write, without its terminator. Used for the fixed TERM_*
 *          escapes above.
 */
static inline void term_write(const char *s) { term_write_n(s, strlen(s)); }

/**
 * @brief Put the terminal into raw mode and switch to the alternate screen.
 *
 * Saves the previous settings first so term_restore() can hand the shell back
 * the way it found it. By default a terminal is in "cooked" mode: the kernel
 * buffers a whole line, lets the user edit it with backspace, echoes it, and
 * only releases it on Enter -- all wrong for a full-screen program, which
 * needs each keystroke the moment it is pressed and owns the whole screen.
 *
 * @note Takes no arguments and returns nothing: it acts on stdin/stdout and
 *       reports no error. If stdin is not a terminal it degrades quietly
 *       rather than failing.
 * @note Disables ISIG, so **Ctrl-C no longer interrupts the process** -- it
 *       arrives as an ordinary 0x03 byte from term_read_keys(). Your input
 *       loop is now responsible for quitting, and for calling term_restore()
 *       on the way out. A crash before that leaves the user's shell in raw
 *       mode.
 * @note Also disables IXON (Ctrl-S/Ctrl-Q flow control, which would look like
 *       a hang), ICRNL (so Ctrl-M and Enter stay distinguishable), ECHO,
 *       ICANON and OPOST (which would rewrite every "\n" to "\r\n" and
 *       corrupt the byte-exact escapes the renderer emits).
 * @note Sets VMIN=0/VTIME=1, so read() returns after at most ~100ms rather
 *       than blocking forever; see term_read_keys().
 *
 * @see term_restore()
 */
static inline void term_init(void) {
#ifdef _WIN32
  // The console equivalent of raw mode: no line buffering, no echo, and
  // Ctrl-C as a byte (PROCESSED_INPUT off). Quick-edit is off too, or a stray
  // click starts a text selection that freezes all output until Esc.
  HANDLE in = GetStdHandle(STD_INPUT_HANDLE), out = GetStdHandle(STD_OUTPUT_HANDLE);
  if (GetConsoleMode(in, &term_saved_in) && GetConsoleMode(out, &term_saved_out)) {
    term_saved_valid = 1;
    DWORD im = term_saved_in & ~(DWORD)(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT |
                                        ENABLE_PROCESSED_INPUT | ENABLE_QUICK_EDIT_MODE);
    SetConsoleMode(in, im | ENABLE_EXTENDED_FLAGS);
    // VT processing is what makes the escapes below mean anything.
    // DISABLE_NEWLINE_AUTO_RETURN is the console's version of clearing OPOST.
    SetConsoleMode(out, term_saved_out | ENABLE_PROCESSED_OUTPUT |
                            ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN);
  }
  FlushConsoleInputBuffer(in);
#else
  // Only mark the saved copy valid if the query worked. If stdin is not a
  // terminal (piped input, a cron job) tcgetattr fails, and restoring garbage
  // termios onto whatever stdin really is would be worse than not restoring.
  if (tcgetattr(STDIN_FILENO, &term_saved) == 0)
    term_saved_valid = 1;

  struct termios raw = term_saved;

  // Input flags:
  //   BRKINT - stop a break condition from raising SIGINT
  //   ICRNL  - stop carriage return being rewritten to newline, so Ctrl-M and
  //            Enter stay distinguishable
  //   INPCK  - disable parity checking (meaningless on a modern terminal)
  //   ISTRIP - stop the 8th bit being stripped off every byte
  //   IXON   - disable Ctrl-S/Ctrl-Q flow control, which would otherwise freeze
  //            output and look like a hang
  raw.c_iflag &= ~(unsigned)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);

  // Output flags: OPOST is all output post-processing. With it on, every "\n"
  // is silently expanded to "\r\n", which would corrupt the byte-exact escape
  // sequences the renderer emits.
  raw.c_oflag &= ~(unsigned)(OPOST);

  // Control flags: 8 bits per byte. This one is a set, not a clear.
  raw.c_cflag |= (unsigned)(CS8);

  // Local flags:
  //   ECHO   - stop keystrokes being printed on top of the rendered frame
  //   ICANON - the important one: read byte-at-a-time instead of line-at-a-time
  //   IEXTEN - disable Ctrl-V literal-next
  //   ISIG   - deliver Ctrl-C/Ctrl-Z as ordinary bytes rather than signals, so
  //            the game decides what quitting means (and can restore the
  //            terminal on the way out)
  raw.c_lflag &= ~(unsigned)(ECHO | ICANON | IEXTEN | ISIG);

  // Read timeout. VMIN is the minimum bytes to return, VTIME a timer in tenths
  // of a second. VMIN=0 with VTIME=1 means "return as soon as any byte arrives,
  // or after 100ms with nothing" -- so read() never blocks forever, and an input
  // loop can re-check a `running` flag between calls instead of hanging on exit.
  // VMIN=0/VTIME=0 would busy-poll; VMIN=1 would block until a key is pressed.
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = (cc_t)term_read_timeout;

  // TCSAFLUSH applies the change once pending output has drained and discards
  // unread input, so keys typed before the game started are not delivered as
  // commands.
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
#endif

  // The alternate screen is a second, scrollback-less buffer. Drawing there
  // means quitting can restore the user's shell history untouched.
  term_write(TERM_ALT_ON);
  term_write(TERM_HIDE_CUR);
  term_write(TERM_CLEAR);
}

/**
 * @brief Undo term_init(), in reverse order.
 *
 * Drops colours before leaving the alternate screen, so no styling leaks onto
 * the restored shell prompt, then puts back the saved termios.
 *
 * @note Safe to call twice: the second call clears attributes again but skips
 *       the tcsetattr, guarded by ::term_saved_valid.
 * @note Safe to call without a matching term_init(); it just restores nothing.
 * @note Call this on *every* exit path, including error paths and signal
 *       handlers, or the user is left in a raw-mode shell with no echo.
 */
static inline void term_restore(void) {
  term_write(TERM_SGR_RESET);
  term_write(TERM_SHOW_CUR);
  term_write(TERM_ALT_OFF);
  if (term_saved_valid) {
#ifdef _WIN32
    SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), term_saved_in | ENABLE_EXTENDED_FLAGS);
    SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), term_saved_out);
#else
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &term_saved);
#endif
    term_saved_valid = 0; // guard against a double restore
  }
}

/**
 * @brief Set how long term_read_keys() waits when no key is queued.
 *
 * @param tenths Tenths of a second, 0..255. 0 makes term_read_keys() return
 *               immediately, which is what a loop paced by ::FrameClock wants;
 *               the default of 1 suits a loop that has no other clock.
 *
 * @note Safe before or after term_init(). On POSIX this is VTIME: before
 *       term_init() it is recorded for term_init() to apply, after it the
 *       live terminal is updated.
 */
static inline void term_set_read_timeout(int tenths) {
  term_read_timeout = tenths < 0 ? 0 : tenths > 255 ? 255 : tenths;
#ifndef _WIN32
  // Before term_init() this only records the value: changing the live
  // terminal now would also change the settings term_restore() puts back.
  struct termios t;
  if (!term_saved_valid || tcgetattr(STDIN_FILENO, &t) != 0)
    return;
  t.c_cc[VMIN] = 0;
  t.c_cc[VTIME] = (cc_t)term_read_timeout;
  tcsetattr(STDIN_FILENO, TCSANOW, &t);
#endif
}
/**
 * @brief Drain every key byte already queued.
 *
 * @param buf Destination buffer for the raw bytes. Not NUL-terminated on
 *            return, and not a string -- one keypress can produce several
 *            bytes (an arrow key arrives as the three bytes "\x1b[A").
 * @param cap Size of @p buf in bytes. Anything beyond this stays queued for
 *            the next call.
 * @return Number of bytes read, 0 if nothing was pressed. Never negative:
 *         read errors are reported as 0.
 *
 * @note Blocks for at most the term_set_read_timeout() value (~100ms by
 *       default), so 0 genuinely means "nothing is being pressed" rather than
 *       "not yet". With a timeout of 0 it never blocks.
 * @note On Windows, console key events are translated to the bytes a POSIX
 *       terminal would send (see term_key_bytes()), so one parser serves both.
 * @note Terminals have no key-up event. A held key shows up as OS auto-repeat,
 *       i.e. repeated bytes arriving on successive calls -- so you cannot ask
 *       whether a key is currently down, only whether it repeated recently.
 */
#ifdef _WIN32
/**
 * @brief The bytes a POSIX terminal sends for one Windows key-down event.
 *
 * @param k   The event. Keys with a character (letters, Enter, Esc, Tab,
 *            Backspace, Ctrl-letter) give that byte; arrows and the editing
 *            keys give their xterm escape sequences.
 * @param out Room for at least 8 bytes.
 * @return Bytes written, 0 for keys with no POSIX equivalent (Shift alone).
 *
 * @note Internal to term_read_keys(). Mapping here rather than relying on
 *       ENABLE_VIRTUAL_TERMINAL_INPUT keeps it working on consoles without VT
 *       input, and gives Backspace the 127 POSIX terminals send.
 */
static inline int term_key_bytes(const KEY_EVENT_RECORD *k, char *out) {
  const char *seq = NULL;
  switch (k->wVirtualKeyCode) {
  case VK_UP: seq = "\x1b[A"; break;
  case VK_DOWN: seq = "\x1b[B"; break;
  case VK_RIGHT: seq = "\x1b[C"; break;
  case VK_LEFT: seq = "\x1b[D"; break;
  case VK_HOME: seq = "\x1b[H"; break;
  case VK_END: seq = "\x1b[F"; break;
  case VK_INSERT: seq = "\x1b[2~"; break;
  case VK_DELETE: seq = "\x1b[3~"; break;
  case VK_PRIOR: seq = "\x1b[5~"; break;
  case VK_NEXT: seq = "\x1b[6~"; break;
  case VK_F1: seq = "\x1bOP"; break;
  case VK_F2: seq = "\x1bOQ"; break;
  case VK_F3: seq = "\x1bOR"; break;
  case VK_F4: seq = "\x1bOS"; break;
  case VK_F5: seq = "\x1b[15~"; break;
  case VK_F6: seq = "\x1b[17~"; break;
  case VK_F7: seq = "\x1b[18~"; break;
  case VK_F8: seq = "\x1b[19~"; break;
  case VK_F9: seq = "\x1b[20~"; break;
  case VK_F10: seq = "\x1b[21~"; break;
  case VK_F11: seq = "\x1b[23~"; break;
  case VK_F12: seq = "\x1b[24~"; break;
  case VK_TAB:
    if (k->dwControlKeyState & SHIFT_PRESSED)
      seq = "\x1b[Z";
    break;
  case VK_BACK: out[0] = 127; return 1;
  }
  if (seq) {
    int n = (int)strlen(seq);
    memcpy(out, seq, (size_t)n);
    return n;
  }
  if (k->uChar.AsciiChar) {
    out[0] = k->uChar.AsciiChar;
    return 1;
  }
  return 0;
}
#endif

static inline int term_read_keys(char *buf, size_t cap) {
#ifdef _WIN32
  HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
  DWORD type = GetFileType(in);
  if (type != FILE_TYPE_CHAR) {
    // Redirected stdin is a pipe or file, which holds plain bytes already.
    // Peek a pipe first: ReadFile on an empty one blocks until it fills.
    DWORD avail = 0, n = 0;
    if (type == FILE_TYPE_PIPE) {
      if (!PeekNamedPipe(in, NULL, 0, NULL, &avail, NULL))
        return 0;
      if (avail == 0) {
        Sleep((DWORD)term_read_timeout * 100);
        return 0;
      }
      if (avail < cap)
        cap = avail;
    }
    if (!ReadFile(in, buf, (DWORD)cap, &n, NULL))
      return 0;
    return (int)n;
  }
  DWORD pending = 0;
  if (!GetNumberOfConsoleInputEvents(in, &pending))
    return 0;
  if (pending == 0) {
    if (WaitForSingleObject(in, (DWORD)term_read_timeout * 100) != WAIT_OBJECT_0)
      return 0;
  }
  // Peek, then remove only the events that fit, so a key that would
  // overflow buf stays queued for the next call like the POSIX read().
  INPUT_RECORD rec[64];
  DWORD got = 0, used = 0;
  if (!PeekConsoleInputA(in, rec, 64, &got))
    return 0;
  size_t n = 0;
  for (; used < got; used++) {
    if (rec[used].EventType != KEY_EVENT || !rec[used].Event.KeyEvent.bKeyDown)
      continue;
    char bytes[8];
    int len = term_key_bytes(&rec[used].Event.KeyEvent, bytes);
    WORD reps = rec[used].Event.KeyEvent.wRepeatCount ? rec[used].Event.KeyEvent.wRepeatCount : 1;
    if (n + (size_t)len * reps > cap)
      break;
    for (WORD r = 0; r < reps; r++) {
      memcpy(buf + n, bytes, (size_t)len);
      n += (size_t)len;
    }
  }
  if (used > 0) {
    DWORD dropped = 0;
    ReadConsoleInputA(in, rec, used, &dropped);
  }
  return (int)n;
#else
  ssize_t n = read(STDIN_FILENO, buf, cap);
  return n > 0 ? (int)n : 0;
#endif
}

/**
 * @brief Absolute-deadline frame pacer.
 *
 * The obvious way to pace a loop is to sleep a fixed amount at the end of each
 * frame. That is wrong, and visibly so: the real interval becomes
 * "render time + delay", so every wobble in render cost shows up as a wobble
 * in frame rate. Rendering a busy frame makes that frame *later*, not shorter.
 *
 * Instead this tracks an absolute deadline and sleeps until it, so render cost
 * is absorbed into the frame's budget rather than added on top. Measured:
 * 0.016ms of jitter at 60fps with 1-9ms of deliberately varied render cost.
 *
 * @var FrameClock::next
 *   Absolute term_now() time the next frame should begin. Stepped by
 *   exactly @ref FrameClock::frame_ns each time, never recomputed from "now",
 *   so rounding errors cannot accumulate into drift.
 * @var FrameClock::frame_ns
 *   Nanoseconds per frame -- the amount `next` steps by. Set from the fps
 *   passed to frame_clock_init().
 */
/**
 * @brief Read the monotonic clock.
 *
 * @param t Out-parameter for the current time. Only differences between two
 *          readings mean anything; the epoch is unspecified.
 *
 * @note CLOCK_MONOTONIC on POSIX, QueryPerformanceCounter on Windows. Use it
 *       for frame timing of your own (an FPS counter, a delta time).
 */
static inline void term_now(struct timespec *t) {
#ifdef _WIN32
  static LARGE_INTEGER freq;
  if (!freq.QuadPart)
    QueryPerformanceFrequency(&freq);
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  t->tv_sec = (time_t)(c.QuadPart / freq.QuadPart);
  t->tv_nsec = (long)((c.QuadPart % freq.QuadPart) * 1000000000LL / freq.QuadPart);
#else
  clock_gettime(CLOCK_MONOTONIC, t);
#endif
}

/**
 * @brief Sleep until an absolute time read from term_now().
 *
 * @param until Deadline. One already in the past returns at once.
 *
 * @note Windows has no absolute sleep, so it sleeps for the remaining time on
 *       a high-resolution waitable timer (Windows 10 1803+). Older systems
 *       fall back to Sleep(), which rounds to the ~15ms scheduler tick.
 */
static inline void term_sleep_until(const struct timespec *until) {
#ifdef _WIN32
  struct timespec now;
  term_now(&now);
  long long ns = (long long)(until->tv_sec - now.tv_sec) * 1000000000LL + (until->tv_nsec - now.tv_nsec);
  if (ns <= 0)
    return;
  static HANDLE timer;
  static int tried;
  if (!tried) {
    tried = 1;
    timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  }
  LARGE_INTEGER due;
  due.QuadPart = -(ns / 100); // negative means relative, in 100ns units
  if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
    WaitForSingleObject(timer, INFINITE);
  else
    Sleep((DWORD)(ns / 1000000));
#else
  clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, until, NULL);
#endif
}

typedef struct FrameClock {
  struct timespec next; // absolute time the next frame should begin
  long frame_ns;        // nanoseconds per frame, the amount `next` steps by
} FrameClock;


/**
 * @brief Start the clock, taking now as the first frame boundary.
 *
 * @param fc  Clock to initialise. Both fields are overwritten.
 * @param fps Target frames per second. A non-positive value falls back to 60
 *            rather than erroring. Note the period is truncated to whole
 *            nanoseconds, so odd rates are approximate.
 *
 * @note Uses the monotonic clock (term_now()), not wall time: it cannot jump backwards
 *       when NTP adjusts the wall clock or the user changes timezone.
 */
static inline void frame_clock_init(FrameClock *fc, int fps) {
  // CLOCK_MONOTONIC, not CLOCK_REALTIME: it cannot jump backwards when NTP
  // adjusts the wall clock or the user changes timezone mid-game.
  fc->frame_ns = fps > 0 ? 1000000000L / fps : 16666667L;
  term_now(&fc->next);
}

/**
 * @brief Block until the next frame boundary.
 *
 * @param fc Clock to wait on. Its deadline is advanced by one frame, or
 *           rebased to now if the last frame overran.
 * @return 1 normally; 0 if the frame just rendered overran its budget. A 0 is
 *         a signal that the loop is not keeping up, which a caller can use to
 *         skip non-essential work and catch back up.
 *
 * @note On overrun the deadline is rebased to now and the cost is taken once,
 *       rather than leaving a stale deadline that would make the next several
 *       frames fire back-to-back trying to "catch up".
 * @note Sleeps with TIMER_ABSTIME, i.e. *until* a point in time rather than
 *       *for* a duration, which closes the race where the time spent computing
 *       the remaining delay is itself not counted.
 */
static inline int frame_wait(FrameClock *fc) {
  struct timespec now;

  // Advance the deadline. timespec keeps seconds and nanoseconds separately and
  // tv_nsec must stay under 1e9, so carry any overflow into tv_sec. `while`
  // rather than `if` so this stays correct for frame_ns over a second.
  fc->next.tv_nsec += fc->frame_ns;
  while (fc->next.tv_nsec >= 1000000000L) {
    fc->next.tv_nsec -= 1000000000L;
    fc->next.tv_sec++;
  }

  // If the deadline is already behind us the frame took longer than its budget.
  // Sleeping is pointless, and leaving the stale deadline in place would make
  // the next several frames fire back-to-back trying to "catch up" -- so rebase
  // to now and take the hit once.
  term_now(&now);
  if (now.tv_sec > fc->next.tv_sec ||
      (now.tv_sec == fc->next.tv_sec && now.tv_nsec > fc->next.tv_nsec)) {
    fc->next = now;
    return 0;
  }

  // TIMER_ABSTIME sleeps *until* a point in time rather than *for* a duration.
  // That closes the race in the relative version, where the time spent
  // computing the remaining delay is itself not counted.
  term_sleep_until(&fc->next);
  return 1;
}
// ---------------------------------------------------------------------------
// Screen -- see the Screen struct below for how the double buffering works
// ---------------------------------------------------------------------------

/**
 * @brief A packed colour, 0x00RRGGBB in a single word.
 *
 * Build one with ::RGB for a foreground, or ::BG / ::BG_NONE for a background;
 * unpack with ::COLOR_R, ::COLOR_G, ::COLOR_B.
 *
 * @note screen_present() compares the colour of every visible cell against the
 *       previous frame, so this is the hottest comparison in the renderer: as
 *       one uint32 it is a single instruction, where a {r,g,b} struct would be
 *       three byte loads and three branches.
 * @note Bit 24 is not part of the colour -- see ::COLOR_SET.
 */
typedef uint32_t Color;

/**
 * @brief Pack three 0..255 channels into a ::Color.
 *
 * @param rr Red channel. Cast through unsigned char, so values outside 0..255
 *           wrap rather than clamp.
 * @param gg Green channel, same treatment.
 * @param bb Blue channel, same treatment.
 * @return A ::Color with bit 24 clear.
 *
 * @note Use this for foregrounds. For a background use ::BG instead, which
 *       also sets the ::COLOR_SET flag -- passing an RGB() value as a
 *       background reads as ::BG_NONE if it happens to be zero.
 * @warning Function-like macro: each argument is evaluated more than once, so
 *          do not pass anything with side effects.
 */
#define RGB(rr, gg, bb)                                                        \
  ((Color)(((uint32_t)(unsigned char)(rr) << 16) |                             \
           ((uint32_t)(unsigned char)(gg) << 8) |                              \
           (uint32_t)(unsigned char)(bb)))

/**
 * @brief Flag in bit 24 marking a background colour as actually set.
 *
 * A packed colour only needs 24 bits, so bit 24 is free to carry a flag. It
 * answers a question RGB alone cannot: "no background" and "a background that
 * happens to be black" are different instructions to the terminal -- one
 * leaves whatever the user's theme uses, the other paints black. Without the
 * flag, bg == 0 would be ambiguous.
 *
 * @note Kept in the spare bit rather than a parallel flags plane so the
 *       per-cell compare still reads one word.
 */
#define COLOR_SET 0x01000000u
/**
 * @brief Pack three 0..255 channels into a background ::Color.
 *
 * @param rr Red channel.
 * @param gg Green channel.
 * @param bb Blue channel.
 * @return ::RGB with ::COLOR_SET applied, so it is distinguishable from
 *         ::BG_NONE even when the colour is pure black.
 *
 * @warning Function-like macro: arguments are evaluated more than once.
 */
#define BG(rr, gg, bb) ((Color)(COLOR_SET | RGB(rr, gg, bb)))
/** @brief "No background": leave whatever the user's terminal theme paints.
 *         Distinct from BG(0,0,0), which actively paints black. */
#define BG_NONE ((Color)0)

/** @brief Extract the red channel (0..255) from a ::Color.
 *  @param c The packed colour. Evaluated more than once. */
#define COLOR_R(c) ((unsigned)(((c) >> 16) & 0xFFu))
/** @brief Extract the green channel (0..255) from a ::Color.
 *  @param c The packed colour. Evaluated more than once. */
#define COLOR_G(c) ((unsigned)(((c) >> 8) & 0xFFu))
/** @brief Extract the blue channel (0..255) from a ::Color. Masks off the
 *         ::COLOR_SET flag along with the other channels.
 *  @param c The packed colour. Evaluated more than once. */
#define COLOR_B(c) ((unsigned)((c) & 0xFFu))


/**
 * @brief How many unchanged cells screen_present() will redraw to bridge a gap
 *        rather than emitting a fresh cursor-position escape.
 *
 * When the scan finds changed cells with a few unchanged ones between them, it
 * can either end the span and pay for a cursor move ("\x1b[12;34H", ~9 bytes),
 * or redraw the unchanged cells and stay put. Below this many it bridges.
 *
 * Which is cheaper depends on colour, and the two cases pull opposite ways. If
 * the bridged cells share the colour around them they cost ~1 byte each and
 * bridging wins easily. If they differ, the bridge also pays a colour escape
 * switching away and another switching back (~38 bytes), and jumping would
 * have been cheaper.
 *
 * Measured over 200 frames of 60 scattered changes on a 200x50 surface, total
 * bytes emitted:
 * @verbatim
 *         gap=0     gap=4     gap=8    gap=16
 *   same  233017    226773    224968   232025   (higher gap better)
 *   diff  436725    440001    446492   467727   (lower gap better)
 * @endverbatim
 * 4 is the compromise: within 1% of optimal in both directions. No setting
 * wins both, so this is not worth tuning unless a specific screen is known to
 * be one case or the other.
 */
#define SCREEN_SPAN_GAP 4

/**
 * @brief Double-buffered terminal drawing surface, structure of arrays,
 *        arena allocated.
 *
 * Two sets of planes: the ones being drawn into, and the ones the terminal is
 * currently showing. screen_present() compares them to work out the smallest
 * set of escape sequences that turns one into the other, emits that in a
 * single write(), then swaps the two sets of pointers.
 *
 * Data is stored as parallel planes rather than an array of cell structs, so
 * the comparison loop walks tight contiguous arrays instead of striding over
 * interleaved fields.
 *
 * @var Screen::w
 *   Surface width in cells. Independent of the terminal window -- present()
 *   clips to whichever is smaller rather than reflowing -- but not fixed:
 *   screen_resize() changes it, and screen_sync_size() tracks the window.
 * @var Screen::h
 *   Surface height in cells, same.
 * @var Screen::n
 *   w * h, cached because every plane is indexed y * w + x.
 * @var Screen::cap_n
 *   Cells the planes were actually allocated for. Always >= @ref Screen::n; a
 *   resize that still fits under this reuses the planes instead of taking
 *   memory from the arena again.
 * @var Screen::arena
 *   Arena the planes came from, kept so a resize past @ref Screen::cap_n can
 *   allocate bigger ones. NULL makes the surface fixed-capacity.
 * @var Screen::mark
 *   Arena offset just past the Screen struct, i.e. where the planes begin.
 *   A grow rewinds here and reallocates, which is only sound because nothing
 *   else may be allocated from the arena after the Screen -- see
 *   screen_resize().
 * @var Screen::characters
 *   Front plane: the glyph in each cell. This is what you draw into.
 * @var Screen::fg
 *   Front plane: each cell's foreground colour.
 * @var Screen::bg
 *   Front plane: each cell's background, or ::BG_NONE for the terminal default.
 * @var Screen::swap_characters
 *   Swap plane: the glyphs the terminal is currently showing.
 * @var Screen::swap_fg
 *   Swap plane: the foregrounds currently shown.
 * @var Screen::swap_bg
 *   Swap plane: the backgrounds currently shown. The diff between the two sets
 *   of planes is exactly what has to be sent.
 * @var Screen::out
 *   Escape-sequence scratch buffer the frame is assembled in, sized for a
 *   worst-case full repaint.
 * @var Screen::out_cap
 *   Size of @ref Screen::out, from screen_out_bytes(). Never exceeded, and
 *   never bounds checked in the inner loop for that reason.
 * @var Screen::prev_valid
 *   False until the first present, and cleared by screen_invalidate(). While
 *   false the swap planes describe nothing and the next frame is a full
 *   repaint rather than a diff.
 * @var Screen::last_cols
 *   Terminal width at the last present, used to detect a resize.
 * @var Screen::last_rows
 *   Terminal height at the last present, same.
 *
 * @warning This name collides with the unrelated `Screen` in raster.h -- the
 *          two headers cannot be included in the same translation unit.
 */
typedef struct Screen {
  // Surface size in cells. This is the size of the drawing surface, and is
  // independent of the terminal window -- present() clips to whichever is
  // smaller rather than reflowing. Changed only through screen_resize().
  int w, h;
  size_t n;     // w * h, cached because every plane is indexed y * w + x
  size_t cap_n; // cells the planes hold; a resize within this reuses them

  // Front planes: what you draw into. Three parallel arrays rather than one
  // array of cells, so present()'s comparison walks each contiguously.
  char *characters; // the glyph in each cell
  Color *fg;        // its foreground colour
  Color *bg;        // its background, or BG_NONE for the terminal's default

  // Swap planes: what the terminal is currently showing. present() swaps these
  // with the front planes, so this is always the last frame actually emitted.
  // The diff between the two sets is exactly what has to be sent.
  char *swap_characters;
  Color *swap_fg;
  Color *swap_bg;

  char *out; // escape-sequence scratch, sized for a worst-case full repaint
  size_t out_cap; // its size, from screen_out_bytes(); never exceeded, and
                  // never bounds checked in the inner loop for that reason

  int prev_valid;           // false until the first present, and cleared by
                            // screen_invalidate(); while false the swap planes
                            // describe nothing and the next frame is a full
                            // repaint rather than a diff
  int last_cols, last_rows; // viewport at last present, to detect a resize

  // Where the planes came from, so screen_resize() can grow them. NULL means
  // fixed capacity: a resize past cap_n fails instead of reallocating.
  Arena *arena;
  size_t mark; // arena offset of the first plane, rewound to on a grow
} Screen;

/**
 * @brief Size of the scratch buffer a frame is assembled in.
 *
 * @param w Surface width in cells.
 * @param h Surface height in cells.
 * @return Bytes required for a worst-case full repaint.
 *
 * @warning This has to be a hard upper bound, not an estimate: present()
 *          writes into the buffer without bounds checks, so under-sizing it is
 *          a buffer overflow.
 * @note Worst case for one cell is every escape firing at once: ~12 for a
 *       cursor move, 19 for a foreground, 19 for a background and 1 for the
 *       character, which is 51; 56 rounds up with room to spare. Per row adds
 *       16 for a leading cursor move, plus 64 overall for the trailing reset
 *       and general slack.
 */
static inline size_t screen_out_bytes(int w, int h) {
  return (size_t)w * (size_t)h * 56 + (size_t)h * 16 + 64;
}

/**
 * @brief Arena bytes the six planes and the scratch buffer take for a given
 *        surface size, excluding the Screen struct itself.
 *
 * @param w Surface width in cells.
 * @param h Surface height in cells.
 * @return Bytes for the planes, the scratch buffer, and worst-case alignment
 *         padding on each of the seven allocations.
 *
 * @note This is what screen_resize() has to fit when it grows, since a grow
 *       rewinds to Screen::mark and reallocates only the planes.
 */
static inline size_t screen_plane_bytes(int w, int h) {
  size_t n = (size_t)w * (size_t)h;
  return 2 * n * sizeof(char) + 4 * n * sizeof(Color) + screen_out_bytes(w, h) +
         7 * ARENA_DEFAULT_ALIGN;
}

/**
 * @brief Total arena bytes screen_create() will consume. Size your arena with
 *        this.
 *
 * @param w Surface width in cells.
 * @param h Surface height in cells.
 * @return Bytes for the Screen struct, all six planes, the scratch buffer, and
 *         worst-case alignment padding on each of the eight allocations.
 *
 * @note An upper bound, not the exact figure: real padding is usually smaller.
 * @note This covers a surface of exactly w by h. A surface that follows the
 *       terminal window needs headroom on top: screen_resize() can only grow
 *       into arena space that is still free, so size the arena for the largest
 *       window you want to support rather than the one you started in.
 */
static inline size_t screen_arena_bytes(int w, int h) {
  return sizeof(Screen) + ARENA_DEFAULT_ALIGN + screen_plane_bytes(w, h);
}

/**
 * @brief Extra cells screen_resize() asks for when it has to grow the planes.
 *
 * Dragging a window edge produces a burst of sizes, each one cell bigger than
 * the last. Growing by exactly what was asked for would rewind and reallocate
 * on every one of them, and each reallocation permanently consumes the slack
 * left over from the previous. Overshooting means a drag usually lands inside
 * the capacity already allocated and costs nothing.
 */
#define SCREEN_GROW_SLACK 16

/**
 * @brief Blank both sets of planes over the current surface.
 *
 * @param s Screen whose planes to reset. Only the first Screen::n cells are
 *          touched; capacity beyond that is never read.
 *
 * @note Used by screen_create_cap() and screen_resize(). After a resize the
 *       old contents are meaningless anyway -- the stride changed, so the same
 *       bytes now describe different cells.
 */
static inline void screen_blank_planes(Screen *s) {
  memset(s->characters, ' ', s->n);
  memset(s->swap_characters, ' ', s->n);
  memset(s->fg, 0, s->n * sizeof(Color));
  memset(s->swap_fg, 0, s->n * sizeof(Color));
  memset(s->bg, 0, s->n * sizeof(Color));
  memset(s->swap_bg, 0, s->n * sizeof(Color));
}

/**
 * @brief Allocate the six planes and the scratch buffer, and point the Screen
 *        at them.
 *
 * @param s      Screen to fill in. Its w/h/n are not touched -- the caller
 *               sets the surface size, this only provides the room.
 * @param a      Arena to allocate from.
 * @param cap_w  Width in cells to allocate for.
 * @param cap_h  Height in cells to allocate for.
 * @return 1 on success, 0 if any allocation did not fit. On failure the
 *         Screen's pointers are left as they were, so the caller decides
 *         whether to rewind.
 *
 * @note Shared by screen_create_cap() and screen_resize(), which is the point:
 *       the two have to lay the planes out identically for a grow to be a
 *       rewind-and-repeat of the original allocation.
 */
static inline int screen_alloc_planes(Screen *s, Arena *a, int cap_w,
                                      int cap_h) {
  size_t cn = (size_t)cap_w * (size_t)cap_h;
  size_t cap = screen_out_bytes(cap_w, cap_h);

  char *ch = (char *)arena_alloc(a, cn);
  char *sch = (char *)arena_alloc(a, cn);
  Color *fg = (Color *)arena_alloc(a, cn * sizeof(Color));
  Color *sfg = (Color *)arena_alloc(a, cn * sizeof(Color));
  Color *bg = (Color *)arena_alloc(a, cn * sizeof(Color));
  Color *sbg = (Color *)arena_alloc(a, cn * sizeof(Color));
  char *out = (char *)arena_alloc(a, cap);

  if (!ch || !sch || !fg || !sfg || !bg || !sbg || !out)
    return 0;

  s->characters = ch;
  s->fg = fg;
  s->bg = bg;
  s->swap_characters = sch;
  s->swap_fg = sfg;
  s->swap_bg = sbg;
  s->out = out;
  s->out_cap = cap;
  s->cap_n = cn;
  return 1;
}

/**
 * @brief Allocate a Screen whose planes are bigger than its starting surface.
 *
 * @param a     Arena to allocate from. Must hold at least
 *              screen_arena_bytes(cap_w, cap_h) free, and must outlive the
 *              Screen.
 * @param w     Starting surface width in cells.
 * @param h     Starting surface height in cells.
 * @param cap_w Width to allocate planes for. Clamped up to @p w.
 * @param cap_h Height to allocate planes for. Clamped up to @p h.
 * @return The new Screen, or NULL if the arena cannot fit it. On failure the
 *         arena is rewound to where it started.
 *
 * @note The capacity is a floor on free resizing, not a ceiling on size:
 *       screen_resize() still grows past it while the arena has room. Its
 *       value is avoiding that reallocation for sizes you expect.
 */
static inline Screen *screen_create_cap(Arena *a, int w, int h, int cap_w,
                                        int cap_h) {
  if (w < 1)
    w = 1;
  if (h < 1)
    h = 1;
  if (cap_w < w)
    cap_w = w;
  if (cap_h < h)
    cap_h = h;

  size_t start = a->current;
  Screen *s = (Screen *)arena_alloc(a, sizeof(Screen));
  if (!s) {
    a->current = start;
    return NULL;
  }

  memset(s, 0, sizeof(*s));
  // Recorded before the planes, so a grow can rewind to exactly here and lay
  // them out again from the same offset.
  s->mark = a->current;
  if (!screen_alloc_planes(s, a, cap_w, cap_h)) {
    a->current = start;
    return NULL;
  }

  s->arena = a;
  s->w = w;
  s->h = h;
  s->n = (size_t)w * (size_t)h;
  s->prev_valid = 0;
  s->last_cols = -1;
  s->last_rows = -1;

  // Both sets start blank. The first present is a full repaint and never reads
  // the swap planes, but leaving them uninitialised would make any future
  // change to that logic read indeterminate memory.
  screen_blank_planes(s);
  return s;
}

/**
 * @brief Allocate a Screen and all its planes from an arena.
 *
 * @param a Arena to allocate from. Must hold at least screen_arena_bytes(w, h)
 *          free. Must outlive the Screen.
 * @param w Surface width in cells. This is the size of your drawing surface,
 *          not the terminal's -- present() clips to the smaller of the two.
 * @param h Surface height in cells, same.
 * @return The new Screen, or NULL if the arena cannot fit it. On failure the
 *         arena is rewound to where it started, so a failed create leaves no
 *         partial allocation behind.
 *
 * @note Everything comes from @p a, so arena_destroy() reclaims the whole
 *       thing at once and there is nothing to free individually.
 * @warning The arena must not be reset while the Screen is in use. The swap
 *          planes in particular have to survive across frames for the
 *          comparison to mean anything.
 * @note Both sets of planes start as blank spaces with zeroed colours.
 */
static inline Screen *screen_create(Arena *a, int w, int h) {
  return screen_create_cap(a, w, h, w, h);
}

/**
 * @brief Change the surface size, reusing or regrowing the planes.
 *
 * Growing past the allocated capacity rewinds the arena to Screen::mark -- the
 * offset just past the Screen struct -- and allocates bigger planes there.
 *
 * @param s Screen to resize. Must not be NULL.
 * @param w New surface width in cells. Values below 1 are clamped to 1.
 * @param h New surface height in cells, same.
 * @return 1 on success, including the no-op case where the size is unchanged.
 *         0 if the planes would have to grow and cannot: either the Screen was
 *         created with no arena, or the arena has no room. The surface is left
 *         untouched and still usable in that case.
 *
 * @warning The rewind is only sound because the planes are the last thing the
 *          Screen allocated. **Nothing allocated from the same arena after
 *          screen_create() may still be live when this grows**, or it is
 *          handed out twice. Give the Screen its own arena, or allocate
 *          everything else before it.
 * @note The contents are discarded, not rescaled: both plane sets come back
 *       blank and the next present is a full repaint. Callers redraw every
 *       frame anyway.
 * @note A shrink clears the terminal, since the rows and columns dropped off
 *       the surface would otherwise keep whatever was last drawn there.
 */
static inline int screen_resize(Screen *s, int w, int h) {
  if (!s)
    return 0;
  if (w < 1)
    w = 1;
  if (h < 1)
    h = 1;
  if (w == s->w && h == s->h)
    return 1;

  size_t n = (size_t)w * (size_t)h;
  if (n > s->cap_n || screen_out_bytes(w, h) > s->out_cap) {
    Arena *a = s->arena;
    if (!a)
      return 0;

    // Check the fit before rewinding, not after: the rewind frees the planes
    // the Screen is still pointing at, so a reallocation that failed
    // afterwards would leave it pointing into space the arena has handed back.
    size_t room = a->capacity - s->mark;
    int gw = w + SCREEN_GROW_SLACK, gh = h + SCREEN_GROW_SLACK;
    if (screen_plane_bytes(gw, gh) > room) {
      gw = w;
      gh = h;
    }
    if (screen_plane_bytes(gw, gh) > room)
      return 0;

    a->current = s->mark;
    if (!screen_alloc_planes(s, a, gw, gh))
      return 0; // unreachable: the fit was just checked against the same bound
  }

  if (w < s->w || h < s->h)
    term_write(TERM_CLEAR);

  s->w = w;
  s->h = h;
  s->n = n;
  screen_blank_planes(s);

  // The swap planes no longer describe the terminal, so the next present must
  // repaint rather than diff.
  s->prev_valid = 0;
  s->last_cols = -1;
  s->last_rows = -1;
  return 1;
}

/**
 * @brief Resize the surface to match the terminal window, if it has changed.
 *
 * Call this once at the top of each frame to make the surface follow the
 * window. It is a cheap ioctl plus a comparison when nothing has changed,
 * which is the common case.
 *
 * @param s Screen to fit to the window. Must not be NULL.
 * @return 1 if the surface changed size, 0 if it did not -- including when the
 *         size query fails, or the planes could not grow. Either way the
 *         surface is left in a drawable state.
 *
 * @note Polling beats a SIGWINCH handler here: the frame loop already wakes at
 *       least every ~100ms (term_read_keys()), and a handler would have to
 *       hand the work back to the loop through a volatile flag anyway to avoid
 *       allocating inside a signal.
 * @note Anything cached from Screen::w or Screen::h is stale after a 1 --
 *       recompute layout when this returns true.
 */
static inline int screen_sync_size(Screen *s) {
  int cols = s->w, rows = s->h;
  if (term_size(&cols, &rows) == -1)
    return 0;
  if (cols == s->w && rows == s->h)
    return 0;
  return screen_resize(s, cols, rows);
}

// --- writes -----------------------------------------------------------------

/**
 * @brief Fill the whole surface with one character and colour.
 *
 * @param s  Screen to clear.
 * @param c  Character to write into every cell, usually ' '.
 * @param fg Foreground for every cell, from ::RGB.
 * @param bg Background for every cell, from ::BG, or ::BG_NONE for the
 *           terminal's default.
 *
 * @note Because present() swaps rather than copies, **every frame must start
 *       here**. The planes you draw into hold the contents of two frames ago,
 *       so anything you do not overwrite is stale, not persistent.
 */
static inline void screen_clear(Screen *s, char c, Color fg, Color bg) {
  memset(s->characters, c, s->n);
  for (size_t i = 0; i < s->n; i++)
    s->fg[i] = fg;
  if (bg == BG_NONE)
    memset(s->bg, 0, s->n * sizeof(Color));
  else
    for (size_t i = 0; i < s->n; i++)
      s->bg[i] = bg;
}

/**
 * @brief Write one cell, character, foreground and background.
 *
 * @param s  Screen to draw into.
 * @param x  Column, 0-based from the left. Out-of-range values are ignored.
 * @param y  Row, 0-based from the top. Out-of-range values are ignored.
 * @param c  Character to write. One byte -- no UTF-8; a multi-byte glyph would
 *           need several cells and break the per-cell diff.
 * @param fg Foreground colour, from ::RGB.
 * @param bg Background colour, from ::BG, or ::BG_NONE for the default.
 *
 * @note Writing off the surface is ignored rather than treated as an error, so
 *       callers can draw sprites that hang off the edge without clipping by
 *       hand. Negative coordinates are caught by the same single comparison,
 *       via an unsigned cast.
 */
static inline void screen_set_bg(Screen *s, int x, int y, char c, Color fg,
                                 Color bg) {
  // Casting to unsigned folds the negative check into the bounds check: -1
  // becomes 4294967295, which is >= any real width. One branch per axis.
  if ((unsigned)x >= (unsigned)s->w || (unsigned)y >= (unsigned)s->h)
    return;
  size_t i = (size_t)y * (size_t)s->w + (size_t)x;
  s->characters[i] = c;
  s->fg[i] = fg;
  s->bg[i] = bg;
}

/**
 * @brief Write one cell, leaving the background as the terminal's default.
 *
 * @param s  Screen to draw into.
 * @param x  Column, 0-based. Out-of-range values are ignored.
 * @param y  Row, 0-based. Out-of-range values are ignored.
 * @param c  Character to write.
 * @param fg Foreground colour, from ::RGB.
 *
 * @see screen_set_bg() to also set a background.
 */
static inline void screen_set(Screen *s, int x, int y, char c, Color fg) {
  screen_set_bg(s, x, y, c, fg, BG_NONE);
}

/**
 * @brief Fill a rectangular block with one character and colour.
 *
 * @param s  Screen to draw into.
 * @param x  Left column of the rect, 0-based. May be negative.
 * @param y  Top row of the rect, 0-based. May be negative.
 * @param w  Width in cells, counted rightwards from @p x. Note this shadows
 *           the meaning of `w` in screen_create() -- here it is the rect, not
 *           the surface. Zero or negative draws nothing.
 * @param h  Height in cells, counted downwards from @p y. Same.
 * @param c  Character to write into every cell of the rect.
 * @param fg Foreground colour, from ::RGB.
 * @param bg Background colour, from ::BG, or ::BG_NONE.
 *
 * @note Clipped to the surface by clamping the loop bounds once, rather than
 *       testing every cell as the single-cell writes do.
 */
static inline void screen_fill_rect(Screen *s, int x, int y, int w, int h,
                                    char c, Color fg, Color bg) {
  int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
  int x1 = x + w > s->w ? s->w : x + w;
  int y1 = y + h > s->h ? s->h : y + h;
  for (int j = y0; j < y1; j++) {
    size_t row = (size_t)j * (size_t)s->w;
    for (int i = x0; i < x1; i++) {
      s->characters[row + (size_t)i] = c;
      s->fg[row + (size_t)i] = fg;
      s->bg[row + (size_t)i] = bg;
    }
  }
}

/**
 * @brief Draw a NUL-terminated string left to right on one row.
 *
 * @param s   Screen to draw into.
 * @param x   Column of the first character. May be negative; the visible tail
 *            still lands correctly.
 * @param y   Row to draw on. A row off the surface draws nothing.
 * @param str String to draw, one byte per cell. Does not wrap at the right
 *            edge -- it is clipped, not continued on the next row.
 * @param fg  Foreground colour, from ::RGB. The background is left as the
 *            terminal's default.
 *
 * @note No formatting. Build the string yourself (snprintf) and pass it here.
 */
static inline void screen_text(Screen *s, int x, int y, const char *str,
                               Color fg) {
  for (int i = 0; str[i]; i++)
    screen_set(s, x + i, y, str[i], fg);
}

// --- reads ------------------------------------------------------------------
// Read back from the front planes -- what is being drawn this frame, not what
// the terminal is showing. Out-of-bounds reads return a blank rather than
// failing, so callers can probe freely.

/**
 * @brief Read back the character at one cell.
 *
 * @param s Screen to read from.
 * @param x Column, 0-based.
 * @param y Row, 0-based.
 * @return The character in that cell, or ' ' if the coordinates are off the
 *         surface -- out-of-bounds reads return a blank rather than failing,
 *         so callers can probe freely.
 *
 * @note Reads the *front* planes: what is being drawn this frame, not what the
 *       terminal is currently showing.
 */
static inline char screen_get(const Screen *s, int x, int y) {
  if ((unsigned)x >= (unsigned)s->w || (unsigned)y >= (unsigned)s->h)
    return ' ';
  return s->characters[(size_t)y * (size_t)s->w + (size_t)x];
}

/**
 * @brief Read back the foreground colour at one cell.
 *
 * @param s Screen to read from.
 * @param x Column, 0-based.
 * @param y Row, 0-based.
 * @return The packed foreground, or 0 (black) if the coordinates are off the
 *         surface. Note 0 is also a legitimate colour, so this cannot be used
 *         to detect out-of-bounds.
 */
static inline Color screen_get_fg(const Screen *s, int x, int y) {
  if ((unsigned)x >= (unsigned)s->w || (unsigned)y >= (unsigned)s->h)
    return 0;
  return s->fg[(size_t)y * (size_t)s->w + (size_t)x];
}

/**
 * @brief Read back the background colour at one cell.
 *
 * @param s Screen to read from.
 * @param x Column, 0-based.
 * @param y Row, 0-based.
 * @return The packed background, or ::BG_NONE if the coordinates are off the
 *         surface. Test against ::BG_NONE, not 0, to tell "no background" from
 *         a black one -- see ::COLOR_SET.
 */
static inline Color screen_get_bg(const Screen *s, int x, int y) {
  if ((unsigned)x >= (unsigned)s->w || (unsigned)y >= (unsigned)s->h)
    return BG_NONE;
  return s->bg[(size_t)y * (size_t)s->w + (size_t)x];
}

// --- present ----------------------------------------------------------------
// The three helpers below append to the frame buffer and return the new write
// position, so a sequence of escapes chains as p = f(p, ...) with no length
// bookkeeping. None bounds check: screen_out_bytes() sized the buffer for the
// worst case precisely so this inner loop does not have to.

/**
 * @brief Append @p v in decimal to the frame buffer.
 *
 * @param p Current write position. Must have room; not bounds checked.
 * @param v Value to format. 0 writes "0" rather than nothing, thanks to the
 *          do/while.
 * @return The new write position, so escapes chain as p = f(p, ...) with no
 *         length bookkeeping.
 *
 * @note Digits come out backwards from the division, hence the scratch buffer
 *       and reverse copy.
 * @note Internal to present(). Like the other appenders it does not bounds
 *       check: screen_out_bytes() sized the buffer for the worst case
 *       precisely so this inner loop does not have to.
 */
static inline char *screen_u32(char *p, unsigned v) {
  char tmp[10];
  int n = 0;
  do {
    tmp[n++] = (char)('0' + v % 10);
    v /= 10;
  } while (v);
  while (n)
    *p++ = tmp[--n];
  return p;
}

/**
 * @brief Append a NUL-terminated string to the frame buffer, without its
 *        terminator.
 *
 * @param p Current write position. Must have room; not bounds checked.
 * @param s String to append.
 * @return The new write position.
 *
 * @note Internal to present().
 */
static inline char *screen_str(char *p, const char *s) {
  while (*s)
    *p++ = *s++;
  return p;
}

/**
 * @brief Append a 24-bit colour escape to the frame buffer.
 *
 * @param p     Current write position. Must have room; not bounds checked.
 * @param intro Which escape to open: "\x1b[38;2;" for a foreground,
 *              "\x1b[48;2;" for a background. The 38/48 codes take a
 *              colour-space selector, and the "2" means direct RGB as opposed
 *              to "5", which would mean a 256-colour palette index.
 * @param c     Colour to emit. Only the low 24 bits are read, so the
 *              ::COLOR_SET flag is harmless here.
 * @return The new write position, past the terminating 'm'.
 *
 * @note Internal to present().
 */
static inline char *screen_emit_color(char *p, const char *intro, Color c) {
  p = screen_str(p, intro);
  p = screen_u32(p, COLOR_R(c));
  *p++ = ';';
  p = screen_u32(p, COLOR_G(c));
  *p++ = ';';
  p = screen_u32(p, COLOR_B(c));
  *p++ = 'm';
  return p;
}

/**
 * @brief Exchange the front and swap planes -- a pointer exchange, not a copy.
 *
 * @param s Screen whose planes to swap.
 *
 * @note Called at the end of present(), once the front planes have been sent
 *       to the terminal and therefore describe what it is now showing.
 */
static inline void screen_swap(Screen *s) {
  char *c = s->characters;
  s->characters = s->swap_characters;
  s->swap_characters = c;

  Color *f = s->fg;
  s->fg = s->swap_fg;
  s->swap_fg = f;

  Color *b = s->bg;
  s->bg = s->swap_bg;
  s->swap_bg = b;
}

/**
 * @brief Draw the frame to the terminal and swap the buffers.
 *
 * @param s Screen to present. NULL, or one whose planes are NULL, returns
 *          silently rather than crashing.
 *
 * @warning Because this **swaps rather than copies**, the planes you draw into
 *          next frame hold the contents of *two* frames ago, not the frame you
 *          just drew. Redraw the full frame each time -- screen_clear() then
 *          draw -- exactly as with any double-buffered renderer. Whatever you
 *          do not overwrite is stale, not persistent.
 *
 * Everything here exists to avoid flicker or wasted bytes:
 * - One write() per frame. Drawing incrementally (a printf per cell) lets the
 *   terminal present half-finished frames, which is what reads as flicker.
 * - No screen clear. Clearing then redrawing shows a blank screen for the
 *   duration of the repaint; changed cells are overwritten in place instead.
 * - Only cells differing from the swap planes are emitted, coalesced into
 *   spans (see ::SCREEN_SPAN_GAP), so a still screen costs zero bytes and zero
 *   syscalls.
 * - Clipping to the terminal viewport. Addressing a row past the bottom clamps
 *   to the last row, so off-screen rows would otherwise pile onto the bottom
 *   line every frame.
 * - Colour escapes only on change, since they are ~20x the size of a cell.
 *
 * @note A resize forces a full repaint, because the terminal reflows or
 *       discards content on its own and the swap planes stop describing it.
 * @note If the size query fails it clips nothing rather than drawing nothing.
 */
static inline void screen_present(Screen *s) {
  if (!s || !s->characters)
    return;

  // If the size query fails, clip nothing rather than drawing nothing.
  int cols = INT_MAX, rows = INT_MAX;
  term_size(&cols, &rows);
  if (cols <= 0)
    cols = INT_MAX;
  if (rows <= 0)
    rows = INT_MAX;

  // The comparison assumes the terminal still shows what the swap planes say.
  // Two things break that: the first present, and a resize (the terminal
  // reflows or discards content on its own). Only the resize needs the clear --
  // on the first present nothing stale is on screen, and clearing would cause
  // the very flicker this is avoiding.
  int full = !s->prev_valid || cols != s->last_cols || rows != s->last_rows;
  if (full && s->prev_valid)
    term_write(TERM_CLEAR);
  s->last_cols = cols;
  s->last_rows = rows;

  // Draw the intersection of the surface and the window. The surface is the
  // size of the game world and does not shrink to fit.
  int vw = s->w < cols ? s->w : cols;
  int vh = s->h < rows ? s->h : rows;

  char *p = s->out;

  // Colour is terminal state, not per-character: once set it applies to every
  // subsequent character until changed. These track what the terminal is
  // currently painting with, so an escape is only emitted when a cell disagrees
  // -- the difference between ~20 bytes per cell and ~1. They start unset each
  // frame because present() ends with an SGR reset.
  int have_fg = 0, have_bg = 0;
  Color cur_fg = 0, cur_bg = 0;

  for (int y = 0; y < vh; y++) {
    size_t base = (size_t)y * (size_t)s->w;
    const char *ch = s->characters + base;
    const Color *fg = s->fg + base;
    const Color *bg = s->bg + base;
    const char *sch = s->swap_characters + base;
    const Color *sfg = s->swap_fg + base;
    const Color *sbg = s->swap_bg + base;

    int x = 0;
    while (x < vw) {
      // Skip to the next cell that actually differs. On a full repaint every
      // cell counts as changed, so this is bypassed.
      if (!full) {
        while (x < vw && ch[x] == sch[x] && fg[x] == sfg[x] && bg[x] == sbg[x])
          x++;
        if (x >= vw)
          break;
      }

      // Find where this span should stop. `end` trails the last genuinely
      // changed cell, `gap` counts unchanged cells since then. Passing
      // SCREEN_SPAN_GAP of them means the next change is far enough away that a
      // new cursor move beats redrawing the gap -- so the span ends at `end`,
      // deliberately not at `i`, leaving those trailing cells out.
      int start = x, end = x, gap = 0;
      for (int i = x; i < vw; i++) {
        if (full || ch[i] != sch[i] || fg[i] != sfg[i] || bg[i] != sbg[i]) {
          end = i;
          gap = 0;
        } else if (++gap > SCREEN_SPAN_GAP) {
          break;
        }
      }

      p = screen_str(p, "\x1b[");
      p = screen_u32(p, (unsigned)y + 1);
      *p++ = ';';
      p = screen_u32(p, (unsigned)start + 1);
      *p++ = 'H';

      for (int i = start; i <= end; i++) {
        if (!have_fg || fg[i] != cur_fg) {
          p = screen_emit_color(p, "\x1b[38;2;", fg[i]);
          cur_fg = fg[i];
          have_fg = 1;
        }
        // Background needs a branch foreground does not: a cell with no
        // background must actively switch back to the default with "49m", or it
        // inherits the previous cell's and smears colour across the row.
        if (bg[i] != BG_NONE) {
          if (!have_bg || bg[i] != cur_bg) {
            p = screen_emit_color(p, "\x1b[48;2;", bg[i]);
            cur_bg = bg[i];
            have_bg = 1;
          }
        } else if (have_bg) {
          p = screen_str(p, "\x1b[49m");
          have_bg = 0;
        }
        *p++ = ch[i];
      }
      x = end + 1;
    }
  }

  // p == s->out means nothing changed anywhere, so there is no write at all.
  // Otherwise the whole frame goes out in one call: the terminal renders what
  // it has received, so a frame delivered in pieces is one the user watches
  // being drawn.
  if (p != s->out) {
    p = screen_str(p, TERM_SGR_RESET);
    term_write_n(s->out, (size_t)(p - s->out));
  }

  // The front planes now describe what the terminal shows, so they become the
  // swap planes. A pointer exchange, not a copy.
  screen_swap(s);
  s->prev_valid = 1;
}

/**
 * @brief Force the next present to repaint every cell.
 *
 * @param s Screen to invalidate. Must not be NULL.
 *
 * @note Use this after something else has written to the terminal behind the
 *       renderer's back, which leaves the swap planes describing a screen
 *       state that is no longer true.
 */
static inline void screen_invalidate(Screen *s) { s->prev_valid = 0; }

// ---------------------------------------------------------------------------
// Keys -- what term_read_keys() hands back for each keypress
// ---------------------------------------------------------------------------

/*
 * Single-byte keys are chars: compare with keys[i] == KEY_ENTER.
 * Multi-byte keys are escape-sequence strings: compare with term_key_match().
 * KEY_ESC is also the first byte of every sequence, so check sequences first.
 */
#define KEY_ESC 0x1b
#define KEY_ENTER '\r'
#define KEY_TAB '\t'
#define KEY_BACKSPACE 0x7f
#define KEY_SPACE ' '
/** @brief Byte for Ctrl+letter, e.g. KEY_CTRL('c') == 0x03. */
#define KEY_CTRL(c) ((char)((c) & 0x1f))

#define KEY_UP_ARROW "\x1b[A"
#define KEY_DOWN_ARROW "\x1b[B"
#define KEY_RIGHT_ARROW "\x1b[C"
#define KEY_LEFT_ARROW "\x1b[D"
#define KEY_HOME "\x1b[H"
#define KEY_END "\x1b[F"
#define KEY_INSERT "\x1b[2~"
#define KEY_DELETE "\x1b[3~"
#define KEY_PAGE_UP "\x1b[5~"
#define KEY_PAGE_DOWN "\x1b[6~"
#define KEY_SHIFT_TAB "\x1b[Z"
#define KEY_F1 "\x1bOP"
#define KEY_F2 "\x1bOQ"
#define KEY_F3 "\x1bOR"
#define KEY_F4 "\x1bOS"
#define KEY_F5 "\x1b[15~"
#define KEY_F6 "\x1b[17~"
#define KEY_F7 "\x1b[18~"
#define KEY_F8 "\x1b[19~"
#define KEY_F9 "\x1b[20~"
#define KEY_F10 "\x1b[21~"
#define KEY_F11 "\x1b[23~"
#define KEY_F12 "\x1b[24~"

/**
 * @brief Check whether a ::KEY_* sequence starts at buf[*i]; if so, step *i
 *        past it.
 *
 * @code
 *     for (int i = 0; i < n; ) {
 *         if (term_key_match(keys, n, &i, KEY_LEFT_ARROW)) x--;
 *         else if (term_key_match(keys, n, &i, KEY_RIGHT_ARROW)) x++;
 *         else if (keys[i++] == 'q') running = 0;
 *     }
 * @endcode
 */
static inline int term_key_match(const char *buf, int n, int *i,
                                 const char *seq) {
  int len = (int)strlen(seq);
  if (*i + len > n || memcmp(buf + *i, seq, (size_t)len) != 0)
    return 0;
  *i += len;
  return 1;
}

#endif

/*
 * MIT License
 * Copyright (c) 2026 Matthew Kabat
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
