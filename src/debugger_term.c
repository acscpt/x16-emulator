// debugger_term.c
//
// Terminal handling and line editor for the stdio debugger. See header.
//
// Commit 1 (foundation): noncanonical / no-echo termios, own echo of
// printable chars, own handling of backspace and Enter. Escape sequences
// (CSI / SS3) are parsed by the state machine but their results are
// silently consumed -- arrow keys, ctrl shortcuts, etc. land in later
// commits.

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
// after the introducer. Commit 1 silently absorbs every completed escape
// sequence -- the dispatch to history navigation / cursor movement lands
// in subsequent commits.
// ---------------------------------------------------------------------------

#define LINE_BUF_SZ 4096

static char   line_buf[LINE_BUF_SZ];
static size_t line_len   = 0;
static bool   line_ready = false;

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
// Lifecycle.
// ---------------------------------------------------------------------------

int term_init(void) {
	is_tty_mode = false;
	line_len    = 0;
	line_ready  = false;
	edit_state  = EDIT_NORMAL;

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
	line_ready = true;
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
			// followed by a final byte in 0x40-0x7E. We don't dispatch
			// anything yet -- commit 2 wires arrow keys into the history
			// ring.
			if (b >= 0x40 && b <= 0x7E) {
				edit_state = EDIT_NORMAL;
			}
			return false;

		case EDIT_SS3:
			// SS3 sequences are ESC O followed by one byte (often the same
			// final letters as CSI). Consume the one byte and return.
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
