// debugger_core.h
//
// SDL-free core of the X16 emulator debugger: state machine, breakpoints,
// step/step-over. Phase 2 of the debugger/SDL decoupling.
//
// Frontends (the SDL TUI in debugger.c today; a stdio frontend in Phase 3)
// drive the core via the functions below and react to state transitions
// through the frontend callbacks (dbg_frontend_on_break / on_resume).
//
// This header must remain free of SDL types and includes.

#ifndef _DEBUGGER_CORE_H
#define _DEBUGGER_CORE_H

#include <stdint.h>
#include <stdbool.h>

// State-machine modes. Integer values preserved from the pre-refactor
// debugger.h DMODE_* constants so external callers are unaffected.
#define DMODE_STOP 0
#define DMODE_STEP 1
#define DMODE_RUN  2

// Breakpoint shape. Preserved verbatim from the pre-refactor debugger.h.
//   pc       16-bit instruction address; -1 means "no breakpoint"
//   bank     CPU program bank (.K); always 0 on 65C02
//   x16Bank  X16 RAM/ROM bank in the $A000-$FFFF window, else -1
struct breakpoint {
	int     pc;
	uint8_t bank;
	int     x16Bank;
};

// Why the emulator entered the stopped state. Passed to on_break() so
// the frontend can present a meaningful prompt or message.
typedef enum {
	DBG_BREAK_NONE,
	DBG_BREAK_USER,           // F12 / `brk` command / SIGINT
	DBG_BREAK_BREAKPOINT,     // user-set breakpoint hit
	DBG_BREAK_STP_OPCODE,     // 6502 STP ($DB) executed
	DBG_BREAK_STEP_COMPLETE,  // single-step or step-over finished
} dbg_break_reason_t;

// What the main loop should do this iteration.
typedef enum {
	DBG_TICK_RUN,   // CPU may step normally
	DBG_TICK_HOLD,  // skip the CPU step (debugger has control)
	DBG_TICK_QUIT,  // emulator should exit
} dbg_tick_t;

// =========================================================================
// Main-loop hook
// =========================================================================

// Called once per main-loop iteration, before the CPU steps. Advances the
// state machine: detects breakpoint hits, completes step / step-over,
// invokes frontend callbacks on state transitions. Does NOT poll input or
// sleep -- the frontend handles that around the call.
dbg_tick_t dbg_tick(void);

// =========================================================================
// State queries
// =========================================================================

// Current mode (DMODE_STOP / DMODE_STEP / DMODE_RUN).
int  dbg_get_mode(void);

// Last stopped CPU PC. Valid when mode != DMODE_RUN. `bank` is .K.
void dbg_get_pc(uint8_t *out_bank, uint16_t *out_addr);

// =========================================================================
// Execution control (frontend -> core)
// =========================================================================

// Force into the stopped state. Idempotent. Triggers on_break(reason, ...).
void dbg_break(dbg_break_reason_t reason);

// Resume normal execution. Triggers on_resume().
void dbg_continue(void);

// Single-step one instruction.
void dbg_step(void);

// Step over JSR/JSL/JML, otherwise single-step.
void dbg_step_over(void);

// Reset the 6502 (matches existing F2 semantics; not a full machine reset).
void dbg_reset_cpu(void);

// =========================================================================
// Breakpoints
// =========================================================================

// Set the single user breakpoint. Replaces any previous value. pc == -1
// clears it. Equivalent to the pre-refactor DEBUGSetBreakPoint().
void dbg_set_breakpoint(struct breakpoint bp);

// Read the current user breakpoint. pc == -1 means "no breakpoint set".
// Used by the SDL frontend's F9 toggle and breakpoint-status render.
struct breakpoint dbg_get_breakpoint(void);

// Clocks elapsed since the CPU last entered the running state (last
// dbg_continue / dbg_step / dbg_step_over). Used by the SDL render for
// the "clocks elapsed" panel display.
uint32_t dbg_clocks_since_resume(void);

// =========================================================================
// Frontend callbacks (frontend defines, core calls)
// =========================================================================
//
// In Phase 2 the SDL frontend (debugger.c) provides these. Phase 3 promotes
// them into a dbg_frontend_t vtable so a second frontend (debugger_stdio.c)
// can register alongside.

extern void dbg_frontend_on_break(dbg_break_reason_t reason, uint8_t bank, uint16_t addr);
extern void dbg_frontend_on_resume(void);

#endif // _DEBUGGER_CORE_H
