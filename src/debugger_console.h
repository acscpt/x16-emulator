// debugger_console.h
//
// Platform console/stdin backend for the stdio debugger.
//
// The shared frontend (debugger_stdio.c) and line editor (debugger_term.c)
// are platform-neutral and reach the operating system only through this
// interface. Concrete implementations live in debugger_console_posix.c and
// debugger_console_win32.c; the build selects exactly one, the same way
// video_win32.c is wired.

#ifndef DEBUGGER_CONSOLE_H
#define DEBUGGER_CONSOLE_H

#include <stdbool.h>
#include <stddef.h>

// Prepare stdin for non-blocking reads. If stdin is an interactive terminal,
// switch it into raw / noncanonical, no-echo mode (and enable ANSI escape
// processing on the output) so the line editor can run, and arrange for the
// terminal to be restored on exit. Returns true if stdin is interactive (the
// line editor is active), false for a pipe or file (byte-stream protocol
// mode). Call once at startup.
bool dbg_console_init(void);

// True when dbg_console_init() found stdin to be an interactive terminal.
bool dbg_console_is_interactive(void);

// Restore the terminal/console to the state saved by dbg_console_init().
// Safe to call when raw mode was never entered, and from a signal handler
// (uses only async-signal-safe calls).
void dbg_console_restore(void);

// Non-blocking read of up to n bytes from stdin into buf. Returns the number
// of bytes read (> 0), 0 if nothing is available right now, or -1 on EOF.
// The "no data" versus "EOF" distinction for a zero-length OS read is handled
// here (a raw tty reports no-data, a pipe reports EOF).
int dbg_console_read(void *buf, size_t n);

// Block up to ms milliseconds waiting for stdin to become readable, so the
// debugger does not busy-spin while stopped and idle.
void dbg_console_wait_readable(int ms);

// Install handlers (POSIX signals / Windows console control handler) that
// restore the terminal and exit cleanly on interrupt, terminate, or hangup.
// Ctrl-C exits; it does not control the emulator (use `brk` for that).
void dbg_console_install_signal_handlers(void);

#endif
