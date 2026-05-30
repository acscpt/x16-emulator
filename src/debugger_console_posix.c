// debugger_console_posix.c
//
// POSIX console backend for the stdio debugger. See debugger_console.h.
//
// Compiled on every non-Windows target. Under Emscripten there is no usable
// stdin, so the whole backend collapses to no-ops (matching the pre-existing
// build-time stubbing of the stdio frontend in the browser).

#include "debugger_console.h"

#if defined(__EMSCRIPTEN__)

bool dbg_console_init(void) { return false; }
bool dbg_console_is_interactive(void) { return false; }
void dbg_console_restore(void) {}
int  dbg_console_read(void *buf, size_t n) { (void)buf; (void)n; return -1; }
void dbg_console_wait_readable(int ms) { (void)ms; }
void dbg_console_install_signal_handlers(void) {}

#else

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

static bool           interactive   = false;
static struct termios saved_termios;
static bool           termios_saved = false;

bool dbg_console_is_interactive(void) {
	return interactive;
}

void dbg_console_restore(void) {
	if (termios_saved) {
		(void)tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
		termios_saved = false;
	}
	interactive = false;
}

static void make_stdin_nonblocking(void) {
	int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
	if (flags >= 0) {
		(void)fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
	}
}

bool dbg_console_init(void) {
	interactive = false;
	make_stdin_nonblocking();

	if (!isatty(STDIN_FILENO)) {
		return false;
	}
	struct termios t;
	if (tcgetattr(STDIN_FILENO, &t) != 0) {
		return false;
	}
	saved_termios = t;
	termios_saved = true;

	// Belt and braces: register the restore as an atexit handler so an
	// abnormal exit (assert, signal default action, exit-on-error in a
	// caller) does not leave the user's shell in noncanonical / no-echo.
	(void)atexit(dbg_console_restore);

	// Noncanonical, no-echo, no auto-translate. We do all line editing.
	t.c_lflag &= ~(ICANON | ECHO | ECHOE | ECHOK | ECHONL);
	t.c_iflag &= ~(ICRNL | INLCR);
	t.c_cc[VMIN]  = 0;
	t.c_cc[VTIME] = 0;
	if (tcsetattr(STDIN_FILENO, TCSANOW, &t) != 0) {
		termios_saved = false;
		return false;
	}
	interactive = true;
	return true;
}

int dbg_console_read(void *buf, size_t n) {
	ssize_t r = read(STDIN_FILENO, buf, n);
	if (r > 0) {
		return (int)r;
	}
	if (r == 0) {
		// In noncanonical mode with VMIN=0/VTIME=0 a 0-byte return means "no
		// data right now", not EOF; a tty in this mode does not deliver EOF
		// through read() at all. On a pipe, 0 bytes is a genuine EOF.
		return interactive ? 0 : -1;
	}
	if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
		return 0;
	}
	return -1;
}

void dbg_console_wait_readable(int ms) {
	struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
	(void)poll(&pfd, 1, ms);
}

// SIGINT / SIGTERM / SIGHUP: restore the user's termios, then re-raise the
// signal with default disposition so the process exits from the signal
// normally (atexit handlers do not run on signal paths). Without this, raw
// mode would leave the shell noncanonical / no-echo. Ctrl-C just exits; it
// does not control the emulator (use `brk` to break in). Only async-signal-
// safe calls are used: tcsetattr (via dbg_console_restore), signal, raise.
static void signal_exit_handler(int sig) {
	dbg_console_restore();
	signal(sig, SIG_DFL);
	raise(sig);
}

void dbg_console_install_signal_handlers(void) {
	(void)signal(SIGINT,  signal_exit_handler);
	(void)signal(SIGTERM, signal_exit_handler);
	(void)signal(SIGHUP,  signal_exit_handler);
}

#endif
