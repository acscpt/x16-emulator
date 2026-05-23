// debugger_core.c
//
// SDL-free core of the X16 emulator debugger. Owns the state machine,
// the single user breakpoint, and the step / step-over machinery.
// Frontends drive the core via the dbg_* functions and react to state
// transitions through the dbg_frontend_t callbacks registered with
// dbg_register_frontend().

#include "debugger_core.h"
#include "memory.h"
#include "timing.h"
#include "cpu/fake6502.h"
#include "cpu/registers.h"

// `regs` is defined in glue.h, but glue.h transitively pulls in SDL --
// which would defeat the point of a SDL-free core. Extern it directly.
extern struct regs regs;

// ----- File-static state (moved here from debugger.c) -----

static int      currentMode      = DMODE_RUN;
static uint32_t debugCPUClocks   = 0;

// "PC at start of current step." Used to detect step completion.
// Distinct from any panel-cursor state the frontend may keep.
static int      step_start_pc         = -1;
static uint8_t  step_start_pc_bank    = 0;
static int      step_start_pc_x16Bank = -1;

// "PC at last stop." Surfaced via dbg_get_pc(). Set when entering STOP.
static int      stopped_pc         = 0;
static uint8_t  stopped_pc_bank    = 0;
static int      stopped_pc_x16Bank = -1;

// Breakpoints. Single user breakpoint plus a one-shot step-breakpoint
// used by step-over to break at the instruction after a JSR/JSL.
static struct breakpoint user_bp = { -1, 0, -1 };
static struct breakpoint step_bp = { -1, 0, -1 };

// Active frontend. NULL until dbg_register_frontend() is called.
static const dbg_frontend_t *active_frontend = NULL;

// ----- Helpers -----

static void notify_break(dbg_break_reason_t r, uint8_t bank, uint16_t pc) {
	if (active_frontend && active_frontend->on_break) {
		active_frontend->on_break(r, bank, pc);
	}
}

static void notify_resume(void) {
	if (active_frontend && active_frontend->on_resume) {
		active_frontend->on_resume();
	}
}

// Returns the X16 RAM/ROM bank visible at `pc` (or -1 outside the
// banked window). Equivalent to debugger.c's static getCurrentBank().
static inline int dbg_x16_bank(int pc, uint8_t bank) {
	int x16Bank = -1;
	if (pc >= 0xA000 && bank == 0) {
		x16Bank = pc < 0xC000 ? memory_get_ram_bank() : memory_get_rom_bank();
	}
	return x16Bank;
}

static inline bool hit_bp(int pc, uint8_t bank, struct breakpoint bp) {
	return (pc == bp.pc) && (bank == bp.bank) && (dbg_x16_bank(pc, bank) == bp.x16Bank);
}

// Transition into the stopped state. Caches the stopped PC and notifies
// the active frontend. Does NOT clear the step breakpoint -- that is only
// cleared when a breakpoint hit (user or step) is what triggers the stop,
// matching pre-refactor behavior at debugger.c:191-193.
static void enter_stop(dbg_break_reason_t reason) {
	currentMode        = DMODE_STOP;
	stopped_pc         = regs.pc;
	stopped_pc_bank    = regs.k;
	stopped_pc_x16Bank = dbg_x16_bank(regs.pc, regs.k);
	notify_break(reason, stopped_pc_bank, (uint16_t)stopped_pc);
}

// ----- Public API -----

dbg_tick_t dbg_tick(void) {
	// Step completion: if regs.pc moved past where we started stepping,
	// the step has run. Matches the pre-refactor "did the CPU move?" check
	// in DEBUGGetCurrentStatus.
	if (currentMode == DMODE_STEP) {
		if (step_start_pc != regs.pc
		    || step_start_pc_bank != regs.k
		    || step_start_pc_x16Bank != dbg_x16_bank(regs.pc, regs.k)) {
			enter_stop(DBG_BREAK_STEP_COMPLETE);
		}
	}

	// Breakpoint detection. Skip if already stopped (matches pre-refactor
	// `currentMode != DMODE_STOP` guard). On a hit, clear the one-shot
	// step breakpoint inline (matches debugger.c:191-193).
	if (currentMode != DMODE_STOP) {
		bool hit_user = hit_bp(regs.pc, regs.k, user_bp);
		bool hit_step = hit_bp(regs.pc, regs.k, step_bp);
		if (hit_user || hit_step) {
			step_bp.pc      = -1;
			step_bp.bank    = 0;
			step_bp.x16Bank = -1;
			enter_stop(hit_user ? DBG_BREAK_BREAKPOINT : DBG_BREAK_STEP_COMPLETE);
		}
	}

	if (currentMode == DMODE_STOP) {
		return DBG_TICK_HOLD;
	}
	return DBG_TICK_RUN;
}

int dbg_get_mode(void) {
	return currentMode;
}

void dbg_get_pc(uint8_t *out_bank, uint16_t *out_addr) {
	if (out_bank) *out_bank = stopped_pc_bank;
	if (out_addr) *out_addr = (uint16_t)stopped_pc;
}

void dbg_break(dbg_break_reason_t reason) {
	if (currentMode == DMODE_STOP) {
		return; // idempotent
	}
	enter_stop(reason);
}

void dbg_continue(void) {
	if (currentMode == DMODE_RUN) {
		return;
	}
	currentMode    = DMODE_RUN;
	debugCPUClocks = clockticks6502;
	timing_init();
	notify_resume();
}

void dbg_step(void) {
	currentMode           = DMODE_STEP;
	step_start_pc         = regs.pc;
	step_start_pc_bank    = regs.k;
	step_start_pc_x16Bank = dbg_x16_bank(regs.pc, regs.k);
	debugCPUClocks        = clockticks6502;
}

void dbg_step_over(void) {
	int opcode = debug_read6502(regs.pc, regs.k, dbg_x16_bank(regs.pc, regs.k));
	if (opcode == 0x20 || opcode == 0xFC || opcode == 0x22) {
		// JSR ($20), JSR (abs,X) on 65C816 ($FC), JSL ($22).
		// Set a one-shot breakpoint at the instruction after the call.
		step_bp.pc      = regs.pc + 3 + (opcode == 0x22);
		step_bp.bank    = regs.k;
		step_bp.x16Bank = dbg_x16_bank(regs.pc, regs.k);
		currentMode     = DMODE_RUN;
		debugCPUClocks  = clockticks6502;
		timing_init();
		notify_resume();
	} else {
		dbg_step();
	}
}

void dbg_reset_cpu(void) {
	reset6502(regs.is65c816);
}

void dbg_set_breakpoint(struct breakpoint bp) {
	user_bp = bp;
}

struct breakpoint dbg_get_breakpoint(void) {
	return user_bp;
}

uint32_t dbg_clocks_since_resume(void) {
	return clockticks6502 - debugCPUClocks;
}

void dbg_register_frontend(const dbg_frontend_t *fe) {
	active_frontend = fe;
}

int dbg_frontend_tick(void) {
	if (active_frontend && active_frontend->tick) {
		return active_frontend->tick();
	}
	return 0;
}
