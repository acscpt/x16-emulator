// debugger_term.h
//
// Terminal handling and line editor for the stdio debugger.
//
// When stdin is a tty, the terminal is switched into noncanonical, no-echo
// mode so the debugger owns line editing (printable echo, backspace, Enter,
// arrow-key history, cursor movement, etc). When stdin is a pipe (testbench,
// script-driven sessions), term_is_tty() returns false and the API is a
// pass-through so the byte-stream protocol path is unchanged.

#ifndef DEBUGGER_TERM_H
#define DEBUGGER_TERM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Initialise the editor state and adopt the console backend's verdict on
// whether stdin is an interactive terminal (see dbg_console_init, which must
// run first). Returns 0 always. term_is_tty() reflects the verdict.
int term_init(void);

// Reset the editor state. The terminal itself is restored by the console
// backend (dbg_console_restore). Safe to call multiple times.
void term_shutdown(void);

// True when term_init() determined that stdin is a tty and the line editor
// is active. False when piped or when init failed to acquire termios.
bool term_is_tty(void);

// Feed one byte from stdin into the editor. Returns true when the byte
// completes a line (Enter); the caller should then call term_take_line.
// In pipe mode (term_is_tty() == false) this is a no-op returning false;
// the caller is expected to handle bytes itself.
bool term_feed_byte(uint8_t b);

// Copy the completed line into `out` (without the terminating newline,
// null-terminated, truncated to outsz-1 if necessary) and reset the editor.
// Returns the length written into `out` excluding the terminator.
size_t term_take_line(char *out, size_t outsz);

// Repaint the prompt and any in-progress line buffer. Used after the
// protocol layer prints asynchronous output mid-edit so the user's
// in-progress input is restored. No-op in pipe mode.
void term_repaint(const char *prompt);

// Erase the current terminal line and place the cursor at column 0.
// Used as the first step of the async-event redraw cycle. No-op in
// pipe mode.
void term_clear_line(void);

#endif
