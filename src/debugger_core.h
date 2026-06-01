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

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// State-machine modes. Integer values preserved from the pre-refactor
// debugger.h DMODE_* constants so external callers are unaffected.
#define DMODE_STOP 0
#define DMODE_STEP 1
#define DMODE_RUN  2

// Maximum length (including terminator) of a condition's source text, kept
// alongside the compiled form for listing.
#define DBG_COND_MAX 80

struct dbg_expr; // condition expression (debugger_expr.h)

// Breakpoint shape. Preserved from the pre-refactor debugger.h, plus an
// optional condition and an enable flag.
//   pc        16-bit instruction address; -1 means "no breakpoint"
//   bank      CPU program bank (.K); always 0 on 65C02
//   x16Bank   X16 RAM/ROM bank in the $A000-$FFFF window, else -1
//   enabled   a disabled breakpoint stays in the table but does not stop the
//             CPU; dbg_breakpoint_add forces this true so callers using a
//             designated initializer get an enabled breakpoint by default
//   cond      compiled `if` condition, or NULL; the breakpoint table owns it
//   cond_src  the condition's source text (for listing), empty if none
struct breakpoint {
	int             pc;
	uint8_t         bank;
	int             x16Bank;
	bool            enabled;
	struct dbg_expr *cond;
	char            cond_src[DBG_COND_MAX];
};

// Why the emulator entered the stopped state. Passed to on_break() so
// the frontend can present a meaningful prompt or message.
typedef enum {
	DBG_BREAK_NONE,
	DBG_BREAK_USER,           // F12 / `brk` command / SIGINT
	DBG_BREAK_BREAKPOINT,     // user-set breakpoint hit
	DBG_BREAK_STP_OPCODE,     // 6502 STP ($DB) executed
	DBG_BREAK_STEP_COMPLETE,  // single-step or step-over finished
	DBG_BREAK_WATCHPOINT,     // memory watchpoint hit
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

// Maximum number of simultaneous user breakpoints.
#define DBG_MAX_BREAKPOINTS 16

// Add a breakpoint to the table, forcing it enabled. Adding one whose
// (pc, bank, x16Bank) is already present replaces the existing entry (its
// condition and enabled state included) rather than ignoring the new one, so
// re-adding with a different `if` clause updates the condition. Returns false
// if bp.pc < 0 or the table is full, true otherwise.
bool dbg_breakpoint_add(struct breakpoint bp);

// Remove the breakpoint matching bp exactly (pc, bank, x16Bank). Returns
// true if one was removed, false if no exact match was present.
bool dbg_breakpoint_remove(struct breakpoint bp);

// Enable or disable the breakpoint at (pc, bank, x16Bank). A disabled
// breakpoint stays in the table but does not stop the CPU. Returns false if no
// breakpoint matches.
bool dbg_breakpoint_set_enabled(int pc, uint8_t bank, int x16Bank, bool enabled);

// Remove every user breakpoint.
void dbg_breakpoint_clear_all(void);

// Number of active user breakpoints (0..DBG_MAX_BREAKPOINTS).
int dbg_breakpoint_count(void);

// Breakpoint at table index idx (0..count-1), in insertion order. Out-of-
// range indices return a cleared breakpoint (pc == -1).
struct breakpoint dbg_breakpoint_get(int idx);

// Legacy single-slot API, retained for the SDL frontend only. dbg_set_
// breakpoint replaces the whole table with the one breakpoint (or clears
// it when pc == -1); dbg_get_breakpoint returns the first entry, or a
// cleared breakpoint when none are set.
void dbg_set_breakpoint(struct breakpoint bp);
struct breakpoint dbg_get_breakpoint(void);

// Clocks elapsed since the CPU last entered the running state (last
// dbg_continue / dbg_step / dbg_step_over). Used by the SDL render for
// the "clocks elapsed" panel display.
uint32_t dbg_clocks_since_resume(void);

// =========================================================================
// Watchpoints (data breakpoints)
// =========================================================================
//
// Stop the CPU when the running program reads or writes a memory address or
// range. The check is hooked into the CPU access paths (memory.c read6502 /
// write6502) and gated by dbg_watch_read_armed / dbg_watch_write_armed so it
// costs nothing when none are armed.

#define DBG_MAX_WATCHPOINTS 16

// A memory watchpoint over the inclusive range [start, end]. x16Bank is the
// X16 RAM/ROM bank for an address in the $A000-$FFFF window, or -1 for the
// unbanked region ($0000-$9FFF), where the bank-select registers are not
// consulted. Ids are slot-stable: the slot index is the id, assigned at
// creation and unchanged until the watchpoint is removed.
struct watchpoint {
	bool             in_use;
	bool             enabled;
	bool             on_read;
	bool             on_write;
	uint16_t         start;
	uint16_t         end;
	int              x16Bank;
	uint64_t         hits;
	struct dbg_expr *cond;            // optional `if` condition, or NULL
	char             cond_src[DBG_COND_MAX];
};

// Add a watchpoint. Returns the slot id (0..DBG_MAX_WATCHPOINTS-1) on
// success, or -1 if the table is full or the request is invalid (no access
// type, or end < start). The id is stable for the watchpoint's lifetime.
// `cond` is an optional compiled condition (the table takes ownership) with
// `cond_src` its source text; pass NULL / "" for none.
int  dbg_watch_add(int x16Bank, uint16_t start, uint16_t end, bool on_read, bool on_write,
                   struct dbg_expr *cond, const char *cond_src);

// Compile a condition expression against the debugger's operand vocabulary
// (registers, flags, mem[...], and in watch context addr/val/is_write/is_read).
// Returns NULL on a parse error, writing a message into errbuf if given.
struct dbg_expr *dbg_compile_condition(const char *src, char *errbuf, size_t errlen);

// Remove the watchpoint in slot id. Returns true if one was present.
bool dbg_watch_remove(int id);

// Remove every watchpoint.
void dbg_watch_clear_all(void);

// Enable or disable the watchpoint in slot id without deleting it. Returns
// true if the slot is in use.
bool dbg_watch_set_enabled(int id, bool enabled);

// Number of watchpoints in use (enabled or not).
int  dbg_watch_count(void);

// Read the watchpoint in slot id into *out. Returns true if the slot is in
// use, false otherwise.
bool dbg_watch_get(int id, struct watchpoint *out);

// Details of the most recent watchpoint hit, valid after a stop with reason
// DBG_BREAK_WATCHPOINT. pc/pc_bank identify the instruction that made the
// access. Returns false if there is no recorded hit.
typedef struct dbg_watch_hit {
	int      id;
	bool     is_write;
	int      x16Bank;
	uint16_t addr;
	uint8_t  value;
	uint16_t pc;
	uint8_t  pc_bank;
} dbg_watch_hit_t;

bool dbg_get_watch_hit(dbg_watch_hit_t *out);

// CPU write-path hook (memory.c write6502). Call only when
// dbg_watch_write_armed is nonzero. On a match it latches a pending stop
// that the next dbg_tick() surfaces; it does not stop mid-instruction.
void dbg_watch_on_write(uint16_t addr, uint8_t value);

// CPU read-path hook (memory.c read6502). Call only when dbg_watch_read_armed
// is nonzero. Mirrors the write hook. The debugger's own reads go through
// real_read6502 (debugOn) and never reach here, so unlike the write path there
// is no poke to suppress.
void dbg_watch_on_read(uint16_t addr, uint8_t value);

// Fast-path guards: counts of enabled write / read watchpoints. memory.c reads
// these directly so the hot path is one load + branch when nothing is armed.
extern int dbg_watch_write_armed;
extern int dbg_watch_read_armed;

// =========================================================================
// State snapshots
// =========================================================================
//
// Both frontends (SDL panels, stdio REPL) consume these instead of reading
// `regs` / `real_read6502` / `video_*` directly. One named entry point per
// data category; the SDL panel renderers and the stdio commands are then
// thin formatters of the same underlying data.

// Full register snapshot. Fields tagged "always" are valid regardless of
// CPU mode; fields tagged "65C816" are only meaningful when is_65c816 is
// true (their values are undefined otherwise).
typedef struct dbg_regs_snapshot {
	// always
	uint16_t pc;
	uint8_t  a;
	uint8_t  xl;
	uint8_t  yl;
	uint16_t sp;
	uint8_t  status;
	uint8_t  k;          // program bank register (PB); 0 in 65C02 mode
	uint8_t  ram_bank;   // currently-selected $A000-$BFFF RAM bank
	uint8_t  rom_bank;   // currently-selected $C000-$FFFF ROM bank
	bool     is_65c816;
	// 65C816 only:
	uint8_t  b;          // 65C816 B accumulator
	uint16_t c;          // 65C816 16-bit C (full accumulator)
	uint16_t x16;        // full 16-bit X
	uint16_t y16;        // full 16-bit Y
	uint8_t  db;         // data bank register (DBR)
	uint16_t dp;         // direct page register
	uint8_t  e;          // 1 = 6502 emulation mode, 0 = native
} dbg_regs_snapshot_t;

void dbg_get_regs(dbg_regs_snapshot_t *out);

// Direct-page register pairs R0..R15. Each entry is a 16-bit word read from
// the direct-page-adjusted addresses ($02+idx*2 lo, $02+idx*2+1 hi).
void dbg_get_zp_pairs(uint16_t out[16]);

// Stack snapshot: N bytes from the top of the stack starting at SP+1,
// matching the SDL stack panel. Wraps at the stack-page boundary.
typedef struct dbg_stack_entry {
	uint16_t addr;
	uint8_t  value;
} dbg_stack_entry_t;

void dbg_get_stack(dbg_stack_entry_t *out, int count);

// VERA state snapshot -- the same fields the SDL VERA panel renders.
typedef struct dbg_vera_snapshot {
	uint32_t addr0;
	uint32_t addr1;
	uint8_t  data0;
	uint8_t  data1;
	uint8_t  ctrl;
	uint8_t  video;
	uint8_t  hscale;
	uint8_t  vscale;
	uint8_t  fxctl;
	uint8_t  fxmul;
	uint8_t  cache[4];
	uint32_t accum;
} dbg_vera_snapshot_t;

void dbg_get_vera(dbg_vera_snapshot_t *out);

// CPU-space memory access. Goes through the same debug-read path the SDL
// panel does, so reads don't perturb VERA / I/O side effects.
//
// x16Bank is the X16 RAM/ROM bank to view in the $A000-$FFFF window.
// Pass -1 (i.e. USE_CURRENT_X16_BANK from memory.h) to read whatever bank
// the CPU has currently selected; pass a specific bank to view that one
// without changing CPU state (matches the SDL "m <bank>:<addr>" feature).
uint8_t dbg_read_mem(uint8_t bank, uint16_t addr, int16_t x16Bank);

// Writes always go through write6502, which targets the currently-selected
// bank.
void    dbg_write_mem(uint8_t bank, uint16_t addr, uint8_t value);
void    dbg_fill_mem(uint8_t bank, uint16_t addr, uint8_t value, uint16_t len);

// Fill RAM / BRAM directly, bypassing write6502 (and therefore I/O
// side-effects in the $9F00-$9FFF range). Mirrors the SDL `f` command's
// behaviour: addresses in ROM range are no-ops, $A000-$BFFF writes go
// to BRAM indexed by x16bank, base RAM is indexed by the 24-bit addr.
// `x16bank < 0` falls back to the CPU's currently-selected RAM bank.
// `incr == 0` is treated as 1.
void    dbg_fill_mem_buffer(uint32_t addr, int x16bank, uint8_t value, uint32_t count, int incr);

// Fill VRAM. Same semantics as repeated dbg_write_vram() but with an
// explicit increment so callers can stride.
void    dbg_fill_vram_buffer(uint32_t addr, uint8_t value, uint32_t count, int incr);

// VRAM byte access (VERA address space, 17-bit).
uint8_t dbg_read_vram(uint32_t addr);
void    dbg_write_vram(uint32_t addr, uint8_t value);

// One disassembled instruction. `byte_count` is the instruction length
// (1..4 for 65C816). `text` is the mnemonic + operands; no leading
// address column (caller formats that). `bytes` is the raw machine code.
typedef struct dbg_disasm_line {
	uint16_t addr;
	uint8_t  bank;
	uint8_t  bytes[4];
	int      byte_count;
	char     text[24];
} dbg_disasm_line_t;

// Returns the instruction's byte_count (call again with addr += result
// to disassemble the next instruction).
int dbg_disasm_line(uint8_t bank, uint16_t addr, dbg_disasm_line_t *out);

// Set a CPU register by name. Names: "pc","a","b","c","d","dp","k",
// "dbr"/"db","x","y","sp","p"/"status","e". Returns false on unknown
// name or out-of-range value.
bool dbg_write_register(const char *name, uint32_t value);

// =========================================================================
// View-cursor state
// =========================================================================
//
// Shared cursor state used by the SDL panel renderer and the stdio REPL.
// The disasm cursor (view_pc) follows the stopped PC after every break
// (matching the pre-refactor SDL on_break behaviour). The data cursor
// (view_data) and the view bank (view_x16bank) are explicit user
// navigation that persists across commands until changed. view_mode
// selects RAM vs VRAM for the data panel; values match the pre-refactor
// DDUMP_* constants so external callers are unaffected.

#define DBG_VIEW_RAM  0
#define DBG_VIEW_VRAM 1

// Disasm cursor: bank + 16-bit addr + X16 RAM/ROM view bank.
// out_x16bank == -1 means "outside the $A000-$FFFF window".
void dbg_get_view_pc(uint8_t *out_bank, uint16_t *out_addr, int *out_x16bank);
void dbg_set_view_pc(uint8_t bank, uint16_t addr, int x16bank);

// Data cursor. 24 bits: high byte is the CPU program bank (used in
// gen2 / 65C816 mode); low 16 bits are the address within that bank.
uint32_t dbg_get_view_data(void);
void     dbg_set_view_data(uint32_t addr);

// View bank for the banked $A000-$FFFF window. -1 = follow the CPU's
// currently-selected RAM/ROM bank; otherwise the panel shows that
// specific X16 bank without changing CPU state.
int  dbg_get_view_x16bank(void);
void dbg_set_view_x16bank(int x16bank);

// DBG_VIEW_RAM or DBG_VIEW_VRAM.
int  dbg_get_view_mode(void);
void dbg_set_view_mode(int mode);

// =========================================================================
// Frontend abstraction
// =========================================================================
//
// A frontend (the SDL TUI in debugger.c, the stdio frontend in
// debugger_stdio.c) packs its callbacks into a dbg_frontend_t and
// registers it with the core. The core invokes them when the state
// machine transitions into or out of the stopped state. Exactly one
// frontend is active at a time; registering a new one replaces the
// previous registration.

typedef struct dbg_frontend {
	// Called once per main-loop iteration when debugger_enabled is true.
	// Return code matches the pre-refactor DEBUGGetCurrentStatus contract:
	//    0: let the CPU step normally this iteration
	//   +1: stay in debug mode (skip the CPU step and loop)
	//   -1: exit the emulator
	int  (*tick)(void);
	// State-transition callbacks.
	void (*on_break)(dbg_break_reason_t reason, uint8_t bank, uint16_t addr);
	void (*on_resume)(void);
} dbg_frontend_t;

void dbg_register_frontend(const dbg_frontend_t *fe);

// Called from main.c's main loop. Delegates to the registered frontend's
// tick() method, or returns 0 if no frontend is registered.
int  dbg_frontend_tick(void);

#endif // _DEBUGGER_CORE_H
