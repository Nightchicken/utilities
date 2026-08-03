#ifndef TERMINAL_H
#define TERMINAL_H
#include "arena.h"
#include "vec.h"
#include <bits/pthreadtypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

// ANSI control sequences. "\x1b[" is CSI (Control Sequence Introducer): the
// terminal reads what follows as a command instead of printing it. A trailing
// "?" parameter marks a private/DEC mode, where 'h' means set and 'l' means
// reset -- that is the only reason ALT_ON and ALT_OFF differ by one letter.
//
// Two more sequences are built by hand in screen_present() rather than being
// macros here, because their numbers come from data:
//   "\x1b[<row>;<col>H"        move cursor (1-based, row first)
//   "\x1b[38;2;<r>;<g>;<b>m"   set foreground to a 24-bit colour
//   "\x1b[48;2;<r>;<g>;<b>m"   same for background ("49m" resets it to default)
// The 38/48 codes take a colour-space selector; "2" means direct RGB, as
// opposed to "5" which would mean a 256-colour palette index.
#define TERM_ALT_ON "\x1b[?1049h"   // switch to the alternate screen buffer
#define TERM_ALT_OFF "\x1b[?1049l"  // switch back, restoring the user's scrollback
#define TERM_HIDE_CUR "\x1b[?25l"   // hide the hardware cursor (it would flicker)
#define TERM_SHOW_CUR "\x1b[?25h"
#define TERM_CLEAR "\x1b[2J"        // erase the whole screen (2 = entire display)
#define TERM_HOME "\x1b[H"          // cursor to row 1, col 1
#define TERM_SGR_RESET "\x1b[0m"    // clear all colour/style attributes

// ---------------------------------------------------------------------------
// Terminal setup
// ---------------------------------------------------------------------------

static struct termios term_saved;
static int term_saved_valid = 0;

// Current terminal size in character cells, via the "get window size" ioctl.
// Returns 0 on success, -1 on error, leaving *cols/*rows untouched on failure so
// the caller's defaults survive.
//
// The ws_col == 0 test matters as much as the -1 test: when stdout is a pipe or
// a pty with no size set, the ioctl can succeed and still hand back zeros. A
// caller that trusted that would compute an empty viewport and draw nothing.
static inline int term_size(int *cols, int *rows) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
    return -1;
  *cols = ws.ws_col;
  *rows = ws.ws_row;
  return 0;
}

// write() is allowed to transfer fewer bytes than asked (a full pipe buffer, or
// a signal arriving mid-call), so a single call is not enough -- the loop keeps
// pushing from where it left off. Giving up on n <= 0 means a closed or broken
// stdout drops the frame instead of spinning forever.
//
// Note this is write(2), not stdio: bypassing FILE* buffering is what lets
// screen_present() guarantee one frame reaches the terminal as one syscall.
static inline void term_write_n(const char *s, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t n = write(STDOUT_FILENO, s + off, len - off);
    if (n <= 0)
      break;
    off += (size_t)n;
  }
}

// term_write_n() for a NUL-terminated string, used for the fixed escapes above.
static inline void term_write(const char *s) { term_write_n(s, strlen(s)); }

// Put the terminal into raw mode for full-screen drawing, saving the previous
// settings so term_restore() can hand the shell back the way it found it.
//
// By default a terminal is in "cooked" mode: the kernel buffers a whole line,
// lets the user edit it with backspace, echoes it, and only releases it on
// Enter -- all wrong for a game, which needs each keystroke the moment it is
// pressed and owns every pixel of the screen itself.
static inline void term_init(void) {
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
  raw.c_cc[VTIME] = 1;

  // TCSAFLUSH applies the change once pending output has drained and discards
  // unread input, so keys typed before the game started are not delivered as
  // commands.
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

  // The alternate screen is a second, scrollback-less buffer. Drawing there
  // means quitting can restore the user's shell history untouched.
  term_write(TERM_ALT_ON);
  term_write(TERM_HIDE_CUR);
  term_write(TERM_CLEAR);
}

// Undo term_init(), in reverse order: drop colours before leaving the alternate
// screen, so no styling leaks onto the restored shell prompt.
static inline void term_restore(void) {
  term_write(TERM_SGR_RESET);
  term_write(TERM_SHOW_CUR);
  term_write(TERM_ALT_OFF);
  if (term_saved_valid) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &term_saved);
    term_saved_valid = 0; // guard against a double restore
  }
}
//------------------------------------------------------------------------------
// Returns number of inputs
// Drain every key byte already queued into `buf`, returning the count. With the
// VMIN=0/VTIME=1 raw mode above this blocks for at most ~100ms, so 0 means
// nothing is being pressed. Terminals have no key-up event: a held key shows up
// as OS auto-repeat, i.e. repeated bytes arriving on successive calls.
//------------------------------------------------------------------------------
static inline int term_read_keys(char *buf, size_t cap) {
  ssize_t n = read(STDIN_FILENO, buf, cap);
  return n > 0 ? (int)n : 0;
}

// ---------------------------------------------------------------------------
// Frame clock
//
// The obvious way to pace a loop is to sleep a fixed amount at the end of each
// frame. That is wrong, and visibly so: the real interval becomes
// "render time + delay", so every wobble in render cost shows up as a wobble in
// frame rate. Rendering a busy frame makes that frame *later*, not shorter.
//
// Instead this tracks an absolute deadline and sleeps until it. Render cost is
// absorbed into the frame's budget rather than added on top, so the interval
// stays flat as long as the work fits. Measured: 0.016ms of jitter at 60fps
// with 1-9ms of deliberately varied render cost per frame.
//
// The deadline is stepped by exactly frame_ns each time, never recomputed from
// "now", so rounding errors cannot accumulate into drift.
// ---------------------------------------------------------------------------
typedef struct FrameClock {
  struct timespec next; // absolute time the next frame should begin
  long frame_ns;        // nanoseconds per frame, the amount `next` steps by
} FrameClock;


// Start the clock at `fps`, taking now as the first frame boundary. A
// non-positive fps falls back to 60.
static inline void frame_clock_init(FrameClock *fc, int fps) {
  // CLOCK_MONOTONIC, not CLOCK_REALTIME: it cannot jump backwards when NTP
  // adjusts the wall clock or the user changes timezone mid-game.
  fc->frame_ns = fps > 0 ? 1000000000L / fps : 16666667L;
  clock_gettime(CLOCK_MONOTONIC, &fc->next);
}

// Block until the next frame boundary. Returns 1 normally, or 0 if the frame
// just rendered overran its budget -- a signal that the loop is not keeping up,
// which a caller can use to skip non-essential work and catch back up.
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
  clock_gettime(CLOCK_MONOTONIC, &now);
  if (now.tv_sec > fc->next.tv_sec ||
      (now.tv_sec == fc->next.tv_sec && now.tv_nsec > fc->next.tv_nsec)) {
    fc->next = now;
    return 0;
  }

  // TIMER_ABSTIME sleeps *until* a point in time rather than *for* a duration.
  // That closes the race in the relative version, where the time spent
  // computing the remaining delay is itself not counted.
  clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &fc->next, NULL);
  return 1;
}
// ---------------------------------------------------------------------------
// Screen
// Used for rendering graphics to terminal
//
// Double buffered, structure of arrays, arena allocated.
//
// Two sets of planes: the ones being drawn into, and the ones the terminal is
// currently showing. present() compares them to work out the smallest set of
// escape sequences that turns one into the other, emits that in a single
// write(), then swaps the two sets of pointers.
//
// Data is stored as parallel planes rather than an array of cell structs, so
// the comparison loop walks three tight contiguous arrays instead of striding
// over interleaved fields.
// ---------------------------------------------------------------------------

// Packed 0x00RRGGBB in a single word. present() compares the colour of every
// visible cell against the previous frame, so this is the hottest comparison in
// the renderer: as one uint32 it is a single instruction, where a {r,g,b}
// struct would be three byte loads and three branches.
typedef uint32_t Color;

#define RGB(rr, gg, bb)                                                        \
  ((Color)(((uint32_t)(unsigned char)(rr) << 16) |                             \
           ((uint32_t)(unsigned char)(gg) << 8) |                              \
           (uint32_t)(unsigned char)(bb)))

// A packed colour only needs 24 bits, so bit 24 is free to carry a flag.
//
// It answers a question RGB alone cannot: "no background" and "a background
// that happens to be black" are different instructions to the terminal -- one
// leaves whatever the user's theme uses, the other paints black. Without the
// flag, bg == 0 would be ambiguous. Keeping it in the spare bit rather than a
// parallel flags plane means the compare still reads one word per cell.
#define COLOR_SET 0x01000000u
#define BG(rr, gg, bb) ((Color)(COLOR_SET | RGB(rr, gg, bb)))
#define BG_NONE ((Color)0)

#define COLOR_R(c) ((unsigned)(((c) >> 16) & 0xFFu))
#define COLOR_G(c) ((unsigned)(((c) >> 8) & 0xFFu))
#define COLOR_B(c) ((unsigned)((c) & 0xFFu))

// When the scan finds changed cells with a few unchanged ones between them, it
// can either end the span and pay for a fresh cursor-position escape
// ("\x1b[12;34H", ~9 bytes), or redraw the unchanged cells and stay put.
// Below this many unchanged cells it bridges the gap instead of jumping.
//
// Which is actually cheaper depends on colour, and the two cases pull opposite
// ways. If the bridged cells share the colour of the cells around them they
// cost ~1 byte each and bridging wins easily. If they differ, the bridge also
// pays a colour escape switching away and another switching back (~38 bytes),
// and jumping would have been cheaper.
//
// Measured over 200 frames of 60 scattered changes on a 200x50 surface, total
// bytes emitted:
//         gap=0     gap=4     gap=8    gap=16
//   same  233017    226773    224968   232025     (higher gap better)
//   diff  436725    440001    446492   467727     (lower gap better)
// 4 is the compromise: within 1% of optimal in both directions. There is no
// setting that wins both, so this is not worth tuning further unless a specific
// screen is known to be one case or the other.
#define SCREEN_SPAN_GAP 4

typedef struct Screen {
  // Surface size in cells. This is the size of the drawing surface, fixed at
  // creation, and is independent of the terminal window -- present() clips to
  // whichever is smaller rather than reflowing.
  int w, h;
  size_t n; // w * h, cached because every plane is indexed y * w + x

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
} Screen;

// Size of the scratch buffer the frame is assembled in. This has to be a hard
// upper bound, not an estimate: present() writes into it without bounds checks,
// so under-sizing it is a buffer overflow.
//
// Worst case for one cell is every escape firing at once:
//   "\x1b[999;999H"           cursor move          ~12
//   "\x1b[38;2;255;255;255m"  foreground            19
//   "\x1b[48;2;255;255;255m"  background            19
//   the character itself                             1
// which is 51; 56 rounds up with room to spare. Per row adds 16 for a leading
// cursor move, plus 64 for the trailing reset and general slack.
static inline size_t screen_out_bytes(int w, int h) {
  return (size_t)w * (size_t)h * 56 + (size_t)h * 16 + 64;
}

// Total arena bytes screen_create() consumes: the Screen, all six planes, the
// scratch buffer, and worst-case alignment padding on each allocation.
// Size your arena with this.
static inline size_t screen_arena_bytes(int w, int h) {
  size_t n = (size_t)w * (size_t)h;
  return sizeof(Screen) + 2 * n * sizeof(char) + 4 * n * sizeof(Color) +
         screen_out_bytes(w, h) + 8 * ARENA_DEFAULT_ALIGN;
}

// The Screen and every plane come from `a`, so arena_destroy() reclaims the
// whole thing at once and there is nothing to free individually. Returns NULL
// if the arena cannot fit it, rewinding the arena to where it started so a
// failed create leaves no partial allocation behind.
//
// The planes live as long as the screen does, so the arena must not be reset
// while it is in use -- the swap planes in particular have to survive across
// frames for the comparison to mean anything.
static inline Screen *screen_create(Arena *a, int w, int h) {
  size_t mark = a->current;
  size_t n = (size_t)w * (size_t)h;
  size_t cap = screen_out_bytes(w, h);

  Screen *s = (Screen *)arena_alloc(a, sizeof(Screen));
  char *ch = (char *)arena_alloc(a, n);
  char *sch = (char *)arena_alloc(a, n);
  Color *fg = (Color *)arena_alloc(a, n * sizeof(Color));
  Color *sfg = (Color *)arena_alloc(a, n * sizeof(Color));
  Color *bg = (Color *)arena_alloc(a, n * sizeof(Color));
  Color *sbg = (Color *)arena_alloc(a, n * sizeof(Color));
  char *out = (char *)arena_alloc(a, cap);

  if (!s || !ch || !sch || !fg || !sfg || !bg || !sbg || !out) {
    a->current = mark;
    return NULL;
  }

  memset(s, 0, sizeof(*s));
  s->w = w;
  s->h = h;
  s->n = n;
  s->characters = ch;
  s->fg = fg;
  s->bg = bg;
  s->swap_characters = sch;
  s->swap_fg = sfg;
  s->swap_bg = sbg;
  s->out = out;
  s->out_cap = cap;
  s->prev_valid = 0;
  s->last_cols = -1;
  s->last_rows = -1;

  // Both sets start blank. The first present is a full repaint and never reads
  // the swap planes, but leaving them uninitialised would make any future
  // change to that logic read indeterminate memory.
  memset(ch, ' ', n);
  memset(sch, ' ', n);
  memset(fg, 0, n * sizeof(Color));
  memset(sfg, 0, n * sizeof(Color));
  memset(bg, 0, n * sizeof(Color));
  memset(sbg, 0, n * sizeof(Color));
  return s;
}

// --- writes -----------------------------------------------------------------

// Fill the whole surface with one character and colour. Because present()
// swaps rather than copies, every frame starts here -- see the note on
// screen_present() about the planes holding two-frames-ago content.
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

// Writing off the surface is ignored rather than treated as an error, so
// callers can draw sprites that hang off the edge without clipping by hand.
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

// One cell, leaving the background as the terminal's default.
static inline void screen_set(Screen *s, int x, int y, char c, Color fg) {
  screen_set_bg(s, x, y, c, fg, BG_NONE);
}

// Fill a w-by-h block. Clipped to the surface by clamping the loop bounds once
// rather than testing every cell, unlike the single-cell writes above.
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

// Draw a NUL-terminated string left to right. Clipped at the surface edge.
static inline void screen_text(Screen *s, int x, int y, const char *str,
                               Color fg) {
  for (int i = 0; str[i]; i++)
    screen_set(s, x + i, y, str[i], fg);
}

// --- reads ------------------------------------------------------------------
// Read back from the front planes -- what is being drawn this frame, not what
// the terminal is showing. Out-of-bounds reads return a blank rather than
// failing, so callers can probe freely.

static inline char screen_get(const Screen *s, int x, int y) {
  if ((unsigned)x >= (unsigned)s->w || (unsigned)y >= (unsigned)s->h)
    return ' ';
  return s->characters[(size_t)y * (size_t)s->w + (size_t)x];
}

static inline Color screen_get_fg(const Screen *s, int x, int y) {
  if ((unsigned)x >= (unsigned)s->w || (unsigned)y >= (unsigned)s->h)
    return 0;
  return s->fg[(size_t)y * (size_t)s->w + (size_t)x];
}

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

// Append `v` in decimal. Digits come out backwards, hence the scratch reverse;
// the do/while means 0 writes "0" rather than nothing.
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

// Append a NUL-terminated string, without the terminator.
static inline char *screen_str(char *p, const char *s) {
  while (*s)
    *p++ = *s++;
  return p;
}

// Append a 24-bit colour escape. `intro` selects which one -- "\x1b[38;2;" for
// foreground, "\x1b[48;2;" for background -- and this completes it with the
// three channel values and the terminating 'm'.
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

// Exchange the front and swap planes. Called at the end of present(), once the
// front planes have been sent to the terminal and therefore describe what it is
// now showing.
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

// Draw the frame and swap the buffers.
//
// IMPORTANT: because this swaps rather than copies, the planes you draw into
// next frame hold the contents of *two* frames ago, not the frame you just
// drew. Redraw the full frame each time -- screen_clear() then draw -- exactly
// as you would with any double-buffered renderer. Whatever you do not overwrite
// is stale, not persistent.
//
// Everything here exists to avoid flicker or wasted bytes:
//   - One write() per frame. Drawing incrementally (a printf per cell) lets the
//     terminal present half-finished frames, which is what reads as flicker.
//   - No screen clear. Clearing then redrawing shows a blank screen for the
//     duration of the repaint; changed cells are overwritten in place instead.
//   - Only cells differing from the swap planes are emitted, coalesced into
//     spans, so a still screen costs zero bytes and zero syscalls.
//   - Clipping to the terminal viewport. Addressing a row past the bottom
//     clamps to the last row, so off-screen rows would otherwise pile onto the
//     bottom line every frame.
//   - Colour escapes only on change, since they are ~20x the size of a cell.
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

// Force the next present to repaint every cell, e.g. after something else has
// written to the terminal behind the renderer's back.
static inline void screen_invalidate(Screen *s) { s->prev_valid = 0; }

#endif









