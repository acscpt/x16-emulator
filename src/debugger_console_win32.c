// debugger_console_win32.c
//
// Windows console backend for the stdio debugger. See debugger_console.h.
//
// STUB. The stdio frontend is still a build-time no-op on Windows
// (debugger_stdio.c prints "not yet supported"), so these are not exercised
// yet; they exist so the shared, platform-neutral debugger_term.c /
// debugger_stdio.c link on Windows with no inline #ifdef. The real
// implementation (SetConsoleMode raw input + VT output, PeekNamedPipe /
// ReadFile non-blocking reads, WaitForSingleObject, SetConsoleCtrlHandler)
// lands in a follow-up.

#include "debugger_console.h"

bool dbg_console_init(void) { return false; }
bool dbg_console_is_interactive(void) { return false; }
void dbg_console_restore(void) {}
int  dbg_console_read(void *buf, size_t n) { (void)buf; (void)n; return -1; }
void dbg_console_wait_readable(int ms) { (void)ms; }
void dbg_console_install_signal_handlers(void) {}
