// debugger_term.c
//
// Terminal handling and line editor for the stdio debugger. See header.
//
// When stdin is a tty, the terminal is switched to noncanonical / no-echo
// and the editor takes over: printable chars are echoed as they arrive,
// backspace erases the previous char, and Enter completes a line and
// hands it to the caller via term_take_line. CSI and SS3 escape
// sequences are parsed and absorbed.

#include "debugger_term.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
#include <termios.h>
#endif

// ---------------------------------------------------------------------------
// Tty / termios state.
// ---------------------------------------------------------------------------

static bool is_tty_mode = false;

#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
static struct termios saved_termios;
static bool           termios_saved = false;
#endif

// ---------------------------------------------------------------------------
// Line buffer + editor state machine.
//
// We sit in EDIT_NORMAL most of the time. When a 0x1B (ESC) byte arrives we
// transition into EDIT_ESC and then EDIT_CSI / EDIT_SS3 as the sequence
// unfolds. CSI sequences end at a "final byte" in the 0x40-0x7E range
// (letters A-Z / a-z / brackets / tilde, etc); SS3 sequences are one byte
// after the introducer. Completed sequences are absorbed without dispatch.
// ---------------------------------------------------------------------------

#define LINE_BUF_SZ 4096

static char   line_buf[LINE_BUF_SZ];
static size_t line_len   = 0;
static bool   line_ready = false;

// ---------------------------------------------------------------------------
// History ring.
//
// Up to HISTORY_MAX entries, oldest evicted first. history_nav tracks where
// the user is currently looking: -1 means "the live line" (line_buf is the
// user's in-progress edit), 0 means "the most recent entry", and so on up
// to history_count - 1 (oldest). When the user leaves the live line to
// walk back into history, the in-progress edit is parked in saved_line so
// down-arrowing past the most recent entry restores it.
// ---------------------------------------------------------------------------

#define HISTORY_MAX 32

static char  *history[HISTORY_MAX];
static int    history_count = 0;
static int    history_first = 0;
static int    history_nav   = -1;
static char   saved_line[LINE_BUF_SZ];
static size_t saved_len = 0;

typedef enum {
	EDIT_NORMAL = 0,
	EDIT_ESC,
	EDIT_CSI,
	EDIT_SS3,
} edit_state_t;

static edit_state_t edit_state = EDIT_NORMAL;

static void write_str(const char *s) {
	(void)write(STDOUT_FILENO, s, strlen(s));
}

static void write_byte(uint8_t b) {
	char c = (char)b;
	(void)write(STDOUT_FILENO, &c, 1);
}

// ---------------------------------------------------------------------------
// History helpers.
// ---------------------------------------------------------------------------

// Visually erase line_buf, then copy `src` (length `n`) in and echo it.
// Used by history navigation to swap the line under the cursor.
static void replace_line(const char *src, size_t n) {
	while (line_len > 0) {
		write_str("\b \b");
		line_len--;
	}
	if (n >= LINE_BUF_SZ) n = LINE_BUF_SZ - 1;
	memcpy(line_buf, src, n);
	line_len = n;
	if (line_len > 0) {
		(void)write(STDOUT_FILENO, line_buf, line_len);
	}
}

// Ring access. offset 0 is the most recent entry, history_count-1 the
// oldest. Returns NULL if out of range.
static const char *history_at(int offset) {
	if (offset < 0 || offset >= history_count) return NULL;
	int idx = (history_first + history_count - 1 - offset) % HISTORY_MAX;
	return history[idx];
}

// Append a line to the ring. No-op for empty lines; every other Enter is
// pushed verbatim so repeated commands (stp stp stp ...) appear in history
// the way they were entered.
static void history_push(const char *line, size_t n) {
	if (n == 0) return;
	int target;
	if (history_count < HISTORY_MAX) {
		target = (history_first + history_count) % HISTORY_MAX;
		history_count++;
	} else {
		free(history[history_first]);
		target        = history_first;
		history_first = (history_first + 1) % HISTORY_MAX;
	}
	char *copy = malloc(n + 1);
	if (!copy) {
		// Out of memory: drop this entry, restore the slot. Rare on a
		// modern desktop, but the bookkeeping has to stay coherent.
		if (history_count > 0 && target == (history_first + history_count - 1) % HISTORY_MAX) {
			history_count--;
		}
		history[target] = NULL;
		return;
	}
	memcpy(copy, line, n);
	copy[n] = '\0';
	history[target] = copy;
}

// Up arrow: walk further back into history, parking the live line on the
// first step away from it.
static void history_up(void) {
	if (history_count == 0) return;
	if (history_nav >= history_count - 1) return;
	if (history_nav == -1) {
		memcpy(saved_line, line_buf, line_len);
		saved_len    = line_len;
		history_nav  = 0;
	} else {
		history_nav++;
	}
	const char *entry = history_at(history_nav);
	if (entry) replace_line(entry, strlen(entry));
}

// Down arrow: walk forward. At the most recent entry, the next press
// restores the parked live line.
static void history_down(void) {
	if (history_nav < 0) return;
	if (history_nav == 0) {
		history_nav = -1;
		replace_line(saved_line, saved_len);
		return;
	}
	history_nav--;
	const char *entry = history_at(history_nav);
	if (entry) replace_line(entry, strlen(entry));
}

// ---------------------------------------------------------------------------
// Lifecycle.
// ---------------------------------------------------------------------------

int term_init(void) {
	is_tty_mode = false;
	line_len    = 0;
	line_ready  = false;
	edit_state  = EDIT_NORMAL;
	history_nav = -1;
	saved_len   = 0;

#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
	if (!isatty(STDIN_FILENO)) {
		return 0;
	}
	struct termios t;
	if (tcgetattr(STDIN_FILENO, &t) != 0) {
		return 0;
	}
	saved_termios = t;
	termios_saved = true;

	// Belt and braces: register the restore as an atexit handler so an
	// abnormal exit (assert, signal default action, exit-on-error in a
	// caller) does not leave the user's shell in noncanonical / no-echo.
	(void)atexit(term_shutdown);

	// Noncanonical, no-echo, no auto-translate. We do all line editing.
	t.c_lflag &= ~(ICANON | ECHO | ECHOE | ECHOK | ECHONL);
	t.c_iflag &= ~(ICRNL | INLCR);
	t.c_cc[VMIN]  = 0;
	t.c_cc[VTIME] = 0;
	if (tcsetattr(STDIN_FILENO, TCSANOW, &t) != 0) {
		termios_saved = false;
		return 0;
	}
	is_tty_mode = true;
#endif
	return 0;
}

void term_shutdown(void) {
#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
	if (termios_saved) {
		(void)tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
		termios_saved = false;
	}
#endif
	is_tty_mode = false;
}

bool term_is_tty(void) {
	return is_tty_mode;
}

// ---------------------------------------------------------------------------
// Byte feed.
// ---------------------------------------------------------------------------

static void submit_line(void) {
	write_byte('\n');
	if (line_len < LINE_BUF_SZ) {
		line_buf[line_len] = '\0';
	} else {
		line_buf[LINE_BUF_SZ - 1] = '\0';
		line_len                  = LINE_BUF_SZ - 1;
	}
	history_push(line_buf, line_len);
	history_nav = -1;
	saved_len   = 0;
	line_ready  = true;
}

static void backspace(void) {
	if (line_len > 0) {
		line_len--;
		write_str("\b \b");
	}
}

static void append_printable(uint8_t b) {
	if (line_len + 1 >= LINE_BUF_SZ) return;
	line_buf[line_len++] = (char)b;
	write_byte(b);
}

bool term_feed_byte(uint8_t b) {
	if (!is_tty_mode) return false;
	if (line_ready) return false; // caller must take_line first

	switch (edit_state) {
		case EDIT_NORMAL:
			if (b == '\n' || b == '\r') {
				submit_line();
				return true;
			}
			if (b == 0x08 || b == 0x7F) {
				backspace();
				return false;
			}
			if (b == 0x1B) {
				edit_state = EDIT_ESC;
				return false;
			}
			if (b >= 0x20 && b < 0x7F) {
				append_printable(b);
				return false;
			}
			// Unhandled control byte: swallow silently for now.
			return false;

		case EDIT_ESC:
			if (b == '[') {
				edit_state = EDIT_CSI;
			} else if (b == 'O') {
				edit_state = EDIT_SS3;
			} else {
				// Bare ESC + byte we don't recognise. Drop back to normal
				// and discard this byte; no two-byte sequence carries
				// meaning at this layer yet.
				edit_state = EDIT_NORMAL;
			}
			return false;

		case EDIT_CSI:
			// CSI bodies are sequences of 0x30-0x3F (digits, ?, ;, etc)
			// followed by a final byte in 0x40-0x7E. A and B are the
			// up and down arrow keys.
			if (b >= 0x40 && b <= 0x7E) {
				if (b == 'A') {
					history_up();
				} else if (b == 'B') {
					history_down();
				}
				edit_state = EDIT_NORMAL;
			}
			return false;

		case EDIT_SS3:
			// SS3 sequences are ESC O followed by one byte (often the same
			// final letters as CSI). Some terminals send ESC O A / ESC O B
			// for the arrow keys when the keypad is in application mode.
			if (b == 'A') {
				history_up();
			} else if (b == 'B') {
				history_down();
			}
			edit_state = EDIT_NORMAL;
			return false;
	}
	return false;
}

size_t term_take_line(char *out, size_t outsz) {
	if (!line_ready || outsz == 0) {
		if (outsz > 0) out[0] = '\0';
		return 0;
	}
	size_t n = line_len;
	if (n >= outsz) n = outsz - 1;
	memcpy(out, line_buf, n);
	out[n] = '\0';

	line_len   = 0;
	line_ready = false;
	return n;
}

void term_repaint(const char *prompt) {
	if (!is_tty_mode) return;
	if (prompt) write_str(prompt);
	if (line_len > 0) {
		(void)write(STDOUT_FILENO, line_buf, line_len);
	}
}
