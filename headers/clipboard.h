#ifndef CLIPBOARD_H
#define CLIPBOARD_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

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
 *
 * @warning The POSIX tools run through popen(), which blocks for as long as
 *          the tool takes (normally a few milliseconds). Call these on a key
 *          press, not every frame.
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

#endif
