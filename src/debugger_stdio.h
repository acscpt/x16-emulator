// debugger_stdio.h
//
// Stdio-based debugger frontend. Implements the wire protocol described in
// untracked/phases/03-stdio-frontend.md (kept locally for now; promotion to
// docs/ happens in Phase 6).
//
// Selected at startup via the -debugstdio CLI flag. Mutually exclusive with
// the SDL TUI frontend (-debug).

#ifndef _DEBUGGER_STDIO_H
#define _DEBUGGER_STDIO_H

// Register the stdio frontend with debugger_core, set stdout line-buffered,
// install any signal handlers. Called from main.c when debugger_stdio_mode
// is true, after memory_init / machine_reset.
void debugger_stdio_init(void);

// Currently a no-op; defined for symmetry / future use.
void debugger_stdio_shutdown(void);

#endif
