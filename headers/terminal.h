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
 * @note POSIX-only (termios, ioctl, clock_nanosleep). Needs `-std=gnu11` or
 *       `-D_POSIX_C_SOURCE=200809L`; strict `-std=c11` hides CLOCK_MONOTONIC
 *       and TIMER_ABSTIME and fails to compile.
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
static struct termios term_saved;
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
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
    return -1;
  *cols = ws.ws_col;
  *rows = ws.ws_row;
  return 0;
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
  while (off < len) {
    ssize_t n = write(STDOUT_FILENO, s + off, len - off);
    if (n <= 0)
      break;
    off += (size_t)n;
  }
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
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &term_saved);
    term_saved_valid = 0; // guard against a double restore
  }
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
 * @note With the VMIN=0/VTIME=1 raw mode set by term_init() this blocks for at
 *       most ~100ms, so 0 genuinely means "nothing is being pressed" rather
 *       than "not yet".
 * @note Terminals have no key-up event. A held key shows up as OS auto-repeat,
 *       i.e. repeated bytes arriving on successive calls -- so you cannot ask
 *       whether a key is currently down, only whether it repeated recently.
 */
static inline int term_read_keys(char *buf, size_t cap) {
  ssize_t n = read(STDIN_FILENO, buf, cap);
  return n > 0 ? (int)n : 0;
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
 *   Absolute CLOCK_MONOTONIC time the next frame should begin. Stepped by
 *   exactly @ref FrameClock::frame_ns each time, never recomputed from "now",
 *   so rounding errors cannot accumulate into drift.
 * @var FrameClock::frame_ns
 *   Nanoseconds per frame -- the amount `next` steps by. Set from the fps
 *   passed to frame_clock_init().
 */
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
 * @note Uses CLOCK_MONOTONIC, not CLOCK_REALTIME: it cannot jump backwards
 *       when NTP adjusts the wall clock or the user changes timezone.
 */
static inline void frame_clock_init(FrameClock *fc, int fps) {
  // CLOCK_MONOTONIC, not CLOCK_REALTIME: it cannot jump backwards when NTP
  // adjusts the wall clock or the user changes timezone mid-game.
  fc->frame_ns = fps > 0 ? 1000000000L / fps : 16666667L;
  clock_gettime(CLOCK_MONOTONIC, &fc->next);
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
 *   Surface width in cells, fixed at creation and independent of the terminal
 *   window -- present() clips to whichever is smaller rather than reflowing.
 * @var Screen::h
 *   Surface height in cells, same.
 * @var Screen::n
 *   w * h, cached because every plane is indexed y * w + x.
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
 * @brief Total arena bytes screen_create() will consume. Size your arena with
 *        this.
 *
 * @param w Surface width in cells.
 * @param h Surface height in cells.
 * @return Bytes for the Screen struct, all six planes, the scratch buffer, and
 *         worst-case alignment padding on each of the eight allocations.
 *
 * @note An upper bound, not the exact figure: real padding is usually smaller.
 */
static inline size_t screen_arena_bytes(int w, int h) {
  size_t n = (size_t)w * (size_t)h;
  return sizeof(Screen) + 2 * n * sizeof(char) + 4 * n * sizeof(Color) +
         screen_out_bytes(w, h) + 8 * ARENA_DEFAULT_ALIGN;
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

#endif









