// debugger_console_win32.c
//
// Windows console backend for the stdio debugger. See debugger_console.h.
//
// Non-blocking stdin on Windows is provided by a worker thread: it does
// blocking ReadFile() on stdin (pipe or console alike) and feeds a ring
// buffer that the main loop drains without blocking. This avoids the
// non-portable peek/poll primitives (PeekNamedPipe is unsupported on a
// pipe under Wine; console handles are not data-signalled for waiting) and
// works the same for a redirected pipe and an interactive console.
//
// For an interactive console, stdin is put into raw mode (no line input /
// echo / processed input, VT input on) and VT processing is enabled on the
// output, so the platform-neutral line editor and its ANSI escapes work.

#include "debugger_console.h"

#include <windows.h>
#include <stdlib.h>
#include <string.h>

// Older mingw-w64 w32api headers may predate this output console-mode flag.
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

static HANDLE h_in  = INVALID_HANDLE_VALUE;
static HANDLE h_out = INVALID_HANDLE_VALUE;
static DWORD  in_type = FILE_TYPE_UNKNOWN;
static bool   interactive = false;

static DWORD saved_in_mode  = 0;
static DWORD saved_out_mode = 0;
static bool  in_mode_saved  = false;
static bool  out_mode_saved = false;

// Reader thread -> ring buffer. ring holds [ring_rd, ring_rd + ring_len).
#define RING_SZ 8192
static unsigned char    ring[RING_SZ];
static size_t           ring_rd  = 0;
static size_t           ring_len = 0;
static CRITICAL_SECTION ring_lock;
static HANDLE           data_event  = NULL; // auto-reset: input arrived or EOF
static bool             reader_eof  = false;
static HANDLE           reader_thread = NULL;

bool dbg_console_is_interactive(void) {
	return interactive;
}

static void push_bytes(const unsigned char *b, size_t n) {
	EnterCriticalSection(&ring_lock);
	for (size_t i = 0; i < n; i++) {
		if (ring_len < RING_SZ) {
			ring[(ring_rd + ring_len) % RING_SZ] = b[i];
			ring_len++;
		}
		// else: ring full; drop. The buffer is far larger than any line.
	}
	LeaveCriticalSection(&ring_lock);
	SetEvent(data_event);
}

static void mark_eof(void) {
	EnterCriticalSection(&ring_lock);
	reader_eof = true;
	LeaveCriticalSection(&ring_lock);
	SetEvent(data_event);
}

// Translate a console key event into the byte stream the platform-neutral
// line editor expects: editor keys become the ANSI escape sequences it
// already parses; every other key contributes its character. We do this
// ourselves rather than relying on ENABLE_VIRTUAL_TERMINAL_INPUT, which
// older Windows consoles and Wine do not honour (arrow keys produce no
// character, so they would otherwise be swallowed).
static void translate_key(const KEY_EVENT_RECORD *k) {
	const char *seq = NULL;
	switch (k->wVirtualKeyCode) {
		case VK_LEFT:   seq = "\x1b[D";  break;
		case VK_RIGHT:  seq = "\x1b[C";  break;
		case VK_UP:     seq = "\x1b[A";  break;
		case VK_DOWN:   seq = "\x1b[B";  break;
		case VK_DELETE: seq = "\x1b[3~"; break;
		default:        break;
	}
	for (WORD r = 0; r < k->wRepeatCount; r++) {
		if (seq) {
			push_bytes((const unsigned char *)seq, strlen(seq));
		} else if (k->uChar.AsciiChar != 0) {
			// Printable chars, Enter (0x0D), backspace (0x08), etc.
			unsigned char b = (unsigned char)k->uChar.AsciiChar;
			push_bytes(&b, 1);
		}
	}
}

// Interactive console: read key-event records and translate them. Ctrl-C is
// intercepted by ENABLE_PROCESSED_INPUT (kept on) and routed to the console
// control handler, so it never arrives here.
static void reader_console_loop(void) {
	for (;;) {
		INPUT_RECORD recs[32];
		DWORD nread = 0;
		if (!ReadConsoleInput(h_in, recs, 32, &nread) || nread == 0) {
			mark_eof();
			return;
		}
		for (DWORD i = 0; i < nread; i++) {
			if (recs[i].EventType == KEY_EVENT && recs[i].Event.KeyEvent.bKeyDown) {
				translate_key(&recs[i].Event.KeyEvent);
			}
		}
	}
}

// Pipe / file: a blocking ReadFile yields a byte stream directly.
static void reader_file_loop(void) {
	for (;;) {
		unsigned char tmp[1024];
		DWORD got = 0;
		if (!ReadFile(h_in, tmp, sizeof(tmp), &got, NULL) || got == 0) {
			mark_eof();
			return;
		}
		push_bytes(tmp, got);
	}
}

static DWORD WINAPI reader_main(LPVOID arg) {
	(void)arg;
	if (in_type == FILE_TYPE_CHAR) {
		reader_console_loop();
	} else {
		reader_file_loop();
	}
	return 0;
}

void dbg_console_restore(void) {
	if (in_mode_saved) {
		SetConsoleMode(h_in, saved_in_mode);
		in_mode_saved = false;
	}
	if (out_mode_saved) {
		SetConsoleMode(h_out, saved_out_mode);
		out_mode_saved = false;
	}
	interactive = false;
}

bool dbg_console_init(void) {
	interactive = false;
	h_in  = GetStdHandle(STD_INPUT_HANDLE);
	h_out = GetStdHandle(STD_OUTPUT_HANDLE);
	in_type = GetFileType(h_in);

	InitializeCriticalSection(&ring_lock);
	data_event = CreateEvent(NULL, FALSE, FALSE, NULL); // auto-reset

	DWORD mode;
	if (in_type == FILE_TYPE_CHAR && GetConsoleMode(h_in, &mode)) {
		// Interactive console: raw key events. Drop line buffering and echo
		// (we echo and edit ourselves), but keep ENABLE_PROCESSED_INPUT so
		// Ctrl-C raises CTRL_C_EVENT for the control handler rather than
		// arriving as a 0x03 byte. Keys are translated via ReadConsoleInput,
		// so VT input mode is deliberately not used.
		saved_in_mode = mode;
		in_mode_saved = true;
		DWORD raw = mode;
		raw &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
		SetConsoleMode(h_in, raw);

		// Enable VT output so the editor's ANSI escapes render.
		DWORD omode;
		if (GetConsoleMode(h_out, &omode)) {
			saved_out_mode = omode;
			out_mode_saved = true;
			SetConsoleMode(h_out, omode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
		}

		// Restore on any normal exit, mirroring the POSIX atexit path.
		(void)atexit(dbg_console_restore);
		interactive = true;
	}

	// The reader thread blocks on ReadFile (pipe or console alike); the
	// process exit tears it down (it has no resources to release).
	reader_thread = CreateThread(NULL, 0, reader_main, NULL, 0, NULL);
	return interactive;
}

int dbg_console_read(void *buf, size_t n) {
	if (n == 0) {
		return 0;
	}
	EnterCriticalSection(&ring_lock);
	size_t avail = ring_len;
	bool   eof   = reader_eof;
	size_t take  = (n < avail) ? n : avail;
	for (size_t i = 0; i < take; i++) {
		((unsigned char *)buf)[i] = ring[ring_rd];
		ring_rd = (ring_rd + 1) % RING_SZ;
		ring_len--;
	}
	LeaveCriticalSection(&ring_lock);

	if (take > 0) {
		return (int)take;
	}
	return eof ? -1 : 0;
}

void dbg_console_wait_readable(int ms) {
	EnterCriticalSection(&ring_lock);
	bool ready = (ring_len > 0) || reader_eof;
	LeaveCriticalSection(&ring_lock);
	if (ready) {
		return;
	}
	WaitForSingleObject(data_event, (DWORD)ms);
}

// Ctrl-C / close / logoff / shutdown: restore the console mode, then return
// FALSE so the default handler runs and the process terminates. Ctrl-C just
// exits; it does not control the emulator (use `brk` to break in).
static BOOL WINAPI ctrl_handler(DWORD type) {
	switch (type) {
		case CTRL_C_EVENT:
		case CTRL_BREAK_EVENT:
		case CTRL_CLOSE_EVENT:
		case CTRL_LOGOFF_EVENT:
		case CTRL_SHUTDOWN_EVENT:
			dbg_console_restore();
			return FALSE;
		default:
			return FALSE;
	}
}

void dbg_console_install_signal_handlers(void) {
	(void)SetConsoleCtrlHandler(ctrl_handler, TRUE);
}
