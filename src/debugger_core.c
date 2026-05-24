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
#include "video.h"
#include "disasm.h"
#include "cpu/fake6502.h"
#include "cpu/registers.h"

#include <string.h>

// `regs` and `RAM` live in glue.h, but glue.h transitively pulls in SDL --
// which would defeat the point of a SDL-free core. Extern them directly.
extern struct regs regs;
extern uint8_t    *RAM;

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

// View-cursor state (see debugger_core.h "View-cursor state" section).
// view_pc == -1 is the lazy-init sentinel; the first access populates
// from regs.pc, matching the pre-refactor SDL `if (currentPC < 0)
// currentPC = regs.pc` behaviour.
static int      view_pc          = -1;
static uint8_t  view_pc_bank     = 0;
static int      view_pc_x16bank  = -1;
static uint32_t view_data        = 0;
static int      view_x16bank     = -1;
static int      view_mode        = DBG_VIEW_RAM;

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

// Transition into the stopped state. Caches the stopped PC, syncs the
// view (disasm) cursor to the new PC, and notifies the active frontend.
// Does NOT clear the step breakpoint -- that is only cleared when a
// breakpoint hit (user or step) is what triggers the stop, matching
// pre-refactor behavior at debugger.c:191-193.
static void enter_stop(dbg_break_reason_t reason) {
	currentMode        = DMODE_STOP;
	stopped_pc         = regs.pc;
	stopped_pc_bank    = regs.k;
	stopped_pc_x16Bank = dbg_x16_bank(regs.pc, regs.k);
	view_pc            = regs.pc;
	view_pc_bank       = regs.k;
	view_pc_x16bank    = stopped_pc_x16Bank;
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

// ----- View-cursor accessors -----

static void view_lazy_init(void) {
	if (view_pc < 0) {
		view_pc         = regs.pc;
		view_pc_bank    = regs.k;
		view_pc_x16bank = dbg_x16_bank(regs.pc, regs.k);
	}
}

void dbg_get_view_pc(uint8_t *out_bank, uint16_t *out_addr, int *out_x16bank) {
	view_lazy_init();
	if (out_bank)    *out_bank    = view_pc_bank;
	if (out_addr)    *out_addr    = (uint16_t)view_pc;
	if (out_x16bank) *out_x16bank = view_pc_x16bank;
}

void dbg_set_view_pc(uint8_t bank, uint16_t addr, int x16bank) {
	view_pc         = addr;
	view_pc_bank    = bank;
	view_pc_x16bank = x16bank;
}

uint32_t dbg_get_view_data(void) {
	return view_data;
}

void dbg_set_view_data(uint32_t addr) {
	view_data = addr;
}

int dbg_get_view_x16bank(void) {
	return view_x16bank;
}

void dbg_set_view_x16bank(int x16bank) {
	view_x16bank = x16bank;
}

int dbg_get_view_mode(void) {
	return view_mode;
}

void dbg_set_view_mode(int mode) {
	view_mode = mode;
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

// =========================================================================
// State snapshots / data access
// =========================================================================

void dbg_get_regs(dbg_regs_snapshot_t *out) {
	out->pc        = regs.pc;
	out->a         = regs.a;
	out->xl        = regs.xl;
	out->yl        = regs.yl;
	out->sp        = regs.sp;
	out->status    = regs.status;
	out->k         = regs.k;
	out->ram_bank  = memory_get_ram_bank();
	out->rom_bank  = memory_get_rom_bank();
	out->is_65c816 = regs.is65c816;
	if (regs.is65c816) {
		out->b   = regs.b;
		out->c   = regs.c;
		out->x16 = regs.x;
		out->y16 = regs.y;
		out->db  = regs.db;
		out->dp  = regs.dp;
		out->e   = regs.e;
	} else {
		out->b   = 0;
		out->c   = 0;
		out->x16 = regs.xl;
		out->y16 = regs.yl;
		out->db  = 0;
		out->dp  = 0;
		out->e   = 1;  // 65C02 is "always" in emulation mode
	}
}

void dbg_get_zp_pairs(uint16_t out[16]) {
	for (int i = 0; i < 16; i++) {
		uint16_t lo_addr = direct_page_add(2 + i * 2);
		uint16_t hi_addr = direct_page_add(2 + i * 2 + 1);
		uint8_t  lo      = real_read6502(lo_addr, 0, true, USE_CURRENT_X16_BANK);
		uint8_t  hi      = real_read6502(hi_addr, 0, true, USE_CURRENT_X16_BANK);
		out[i]           = (uint16_t)((hi << 8) | lo);
	}
}

void dbg_get_stack(dbg_stack_entry_t *out, int count) {
	uint16_t sp = regs.sp;
	increment_wrap_at_page_boundary(&sp);
	for (int i = 0; i < count; i++) {
		out[i].addr  = sp;
		out[i].value = real_read6502(sp, 0, true, USE_CURRENT_X16_BANK);
		increment_wrap_at_page_boundary(&sp);
	}
}

void dbg_get_vera(dbg_vera_snapshot_t *out) {
	out->addr0    = video_get_address(0);
	out->addr1    = video_get_address(1);
	out->data0    = video_read(3, true);
	out->data1    = video_read(4, true);
	out->ctrl     = video_read(5, true);
	out->video    = video_get_dc_value(0);
	out->hscale   = video_get_dc_value(1);
	out->vscale   = video_get_dc_value(2);
	out->fxctl    = video_get_dc_value(8);
	out->fxmul    = video_get_dc_value(11);
	out->cache[0] = video_get_dc_value(24);
	out->cache[1] = video_get_dc_value(25);
	out->cache[2] = video_get_dc_value(26);
	out->cache[3] = video_get_dc_value(27);
	out->accum    = video_get_fx_accum();
}

uint8_t dbg_read_mem(uint8_t bank, uint16_t addr, int16_t x16Bank) {
	return real_read6502(addr, bank, true, x16Bank);
}

void dbg_write_mem(uint8_t bank, uint16_t addr, uint8_t value) {
	write6502(addr, bank, value);
}

void dbg_fill_mem(uint8_t bank, uint16_t addr, uint8_t value, uint16_t len) {
	for (uint16_t i = 0; i < len; i++) {
		write6502((uint16_t)(addr + i), bank, value);
	}
}

uint8_t dbg_read_vram(uint32_t addr) {
	return video_space_read(addr);
}

void dbg_write_vram(uint32_t addr, uint8_t value) {
	video_space_write(addr, value);
}

int dbg_disasm_line(uint8_t bank, uint16_t addr, dbg_disasm_line_t *out) {
	out->addr = addr;
	out->bank = bank;
	int32_t eff_addr;
	int     len = disasm(addr, bank, RAM, out->text, sizeof(out->text),
	                     -1, regs.status, &eff_addr);
	out->byte_count = len > 4 ? 4 : len;
	for (int i = 0; i < out->byte_count; i++) {
		out->bytes[i] = real_read6502((uint16_t)(addr + i), bank, true, -1);
	}
	return len;
}

bool dbg_write_register(const char *name, uint32_t value) {
	if (!name) return false;
	if      (!strcmp(name, "pc"))                              { regs.pc     = (uint16_t)value; }
	else if (!strcmp(name, "a"))                               { regs.a      = (uint8_t)value;  }
	else if (!strcmp(name, "b"))                               { regs.b      = (uint8_t)value;  }
	else if (!strcmp(name, "c"))                               { regs.c      = (uint16_t)value; }
	else if (!strcmp(name, "x"))                               {
		if (regs.is65c816) regs.x  = (uint16_t)value;
		else               regs.xl = (uint8_t)value;
	}
	else if (!strcmp(name, "y"))                               {
		if (regs.is65c816) regs.y  = (uint16_t)value;
		else               regs.yl = (uint8_t)value;
	}
	else if (!strcmp(name, "sp"))                              { regs.sp     = (uint16_t)value; }
	else if (!strcmp(name, "p") || !strcmp(name, "status"))    { regs.status = (uint8_t)value;  }
	else if (!strcmp(name, "k"))                               { regs.k      = (uint8_t)value;  }
	else if (!strcmp(name, "db") || !strcmp(name, "dbr"))      { regs.db     = (uint8_t)value;  }
	else if (!strcmp(name, "d")  || !strcmp(name, "dp"))       { regs.dp     = (uint16_t)value; }
	else if (!strcmp(name, "e"))                               { regs.e      = value ? 1 : 0;   }
	else return false;
	return true;
}
