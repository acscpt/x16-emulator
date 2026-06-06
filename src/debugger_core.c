// debugger_core.c
//
// SDL-free core of the X16 emulator debugger. Owns the state machine,
// the single user breakpoint, and the step / step-over machinery.
// Frontends drive the core via the dbg_* functions and react to state
// transitions through the dbg_frontend_t callbacks registered with
// dbg_register_frontend().

#include "debugger_core.h"
#include "debugger_expr.h"
#include "memory.h"
#include "timing.h"
#include "video.h"
#include "disasm.h"
#include "cpu/fake6502.h"
#include "cpu/registers.h"

#include <string.h>

// `regs`, `RAM`, `BRAM`, and `num_banks` live in glue.h, but glue.h
// transitively pulls in SDL -- which would defeat the point of a SDL-free
// core. Extern them directly.
extern struct regs  regs;
extern uint8_t     *RAM;
extern uint8_t     *BRAM;
extern uint16_t     num_banks;

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

// Breakpoints. A fixed table of user breakpoints plus a one-shot
// step-breakpoint used by step-over to break at the instruction after a
// JSR/JSL. user_bp[0..user_bp_count-1] are the active entries, in the
// order they were added.
static struct breakpoint user_bp[DBG_MAX_BREAKPOINTS];
static int               user_bp_count = 0;
static struct breakpoint step_bp = { -1, 0, -1 };

// Exact-match index of (pc, bank, x16Bank) in the user table, or -1.
static int find_user_bp(int pc, uint8_t bank, int x16Bank) {
	for (int i = 0; i < user_bp_count; i++) {
		if (user_bp[i].pc == pc && user_bp[i].bank == bank && user_bp[i].x16Bank == x16Bank) {
			return i;
		}
	}
	return -1;
}

// Watchpoints. Slot index is the id; entries persist in their slot until
// removed. dbg_watch_{write,read}_armed are the counts of enabled write and
// read watchpoints, read directly by memory.c's write6502 / read6502 as
// hot-path guards.
static struct watchpoint watch_table[DBG_MAX_WATCHPOINTS];
int dbg_watch_write_armed = 0;
int dbg_watch_read_armed  = 0;

// A watchpoint hit latched by the read/write access hook during step6502,
// surfaced by the next dbg_tick(). have_watch_hit keeps the last hit readable
// by the frontend after the stop (for the `* WP` event).
static bool            watch_pending  = false;
static dbg_watch_hit_t pending_hit;
static dbg_watch_hit_t last_hit;
static bool            have_watch_hit = false;

// Set around debugger pokes (dbg_write_mem / dbg_fill_mem) so their writes,
// which also go through write6502, do not trip watchpoints. Only genuine
// CPU writes should fire them. Reads need no equivalent: the debugger reads
// through real_read6502 (debugOn), never the hooked public read6502.
static bool            watch_suppress = false;

// PC of the instruction about to execute, snapshotted before each step so
// the access hook can report which instruction made the read or write (regs.pc
// has already advanced by the time write6502 / read6502 runs).
static uint16_t cur_instr_pc   = 0;
static uint8_t  cur_instr_bank = 0;

static void recompute_watch_armed(void) {
	int nw = 0, nr = 0;
	for (int i = 0; i < DBG_MAX_WATCHPOINTS; i++) {
		if (!watch_table[i].in_use || !watch_table[i].enabled) {
			continue;
		}
		if (watch_table[i].on_write) nw++;
		if (watch_table[i].on_read)  nr++;
	}
	dbg_watch_write_armed = nw;
	dbg_watch_read_armed  = nr;
}

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

// ----- Condition expressions -----

// Operand context for evaluating a condition. The register snapshot is taken
// once before eval; the watch fields carry the current access (zero for a
// breakpoint condition, where addr/val/is_write resolve to 0).
struct cond_ctx {
	dbg_regs_snapshot_t r;
	uint16_t wp_addr;
	uint8_t  wp_val;
	bool     wp_is_write;
	bool     wp_is_read;
};

static const char *const cond_vars[] = {
	"a", "x", "y", "sp", "pc", "p", "n", "v", "z", "c", "i", "d",
	"addr", "val", "is_read", "is_write", "mem", NULL,
};

static bool cond_valid(void *ctx, const char *name) {
	(void)ctx;
	for (int i = 0; cond_vars[i]; i++) {
		if (!strcmp(cond_vars[i], name)) return true;
	}
	return false;
}

static int64_t cond_resolve(void *ctx, const char *name, bool indexed, int64_t index) {
	struct cond_ctx *c = ctx;
	if (indexed) {
		// mem[addr]: a current-bank CPU-space read.
		return dbg_read_mem(0, (uint16_t)index, -1);
	}
	uint8_t p = c->r.status;
	if (!strcmp(name, "a"))        return c->r.a;
	if (!strcmp(name, "x"))        return c->r.xl;
	if (!strcmp(name, "y"))        return c->r.yl;
	if (!strcmp(name, "sp"))       return c->r.sp;
	if (!strcmp(name, "pc"))       return c->r.pc;
	if (!strcmp(name, "p"))        return p;
	if (!strcmp(name, "n"))        return (p >> 7) & 1;
	if (!strcmp(name, "v"))        return (p >> 6) & 1;
	if (!strcmp(name, "d"))        return (p >> 3) & 1;
	if (!strcmp(name, "i"))        return (p >> 2) & 1;
	if (!strcmp(name, "z"))        return (p >> 1) & 1;
	if (!strcmp(name, "c"))        return p & 1;
	if (!strcmp(name, "addr"))     return c->wp_addr;
	if (!strcmp(name, "val"))      return c->wp_val;
	if (!strcmp(name, "is_write")) return c->wp_is_write ? 1 : 0;
	if (!strcmp(name, "is_read"))  return c->wp_is_read ? 1 : 0;
	return 0;
}

struct dbg_expr *dbg_compile_condition(const char *src, char *errbuf, size_t errlen) {
	return dbg_expr_compile(src, cond_valid, NULL, errbuf, errlen);
}

// Evaluate a condition. NULL means "always true".
static bool cond_passes(struct dbg_expr *cond, bool have_access, uint16_t addr, uint8_t val, bool is_write) {
	if (!cond) {
		return true;
	}
	struct cond_ctx c;
	memset(&c, 0, sizeof(c));
	dbg_get_regs(&c.r);
	if (have_access) {
		c.wp_addr     = addr;
		c.wp_val      = val;
		c.wp_is_write = is_write;
		c.wp_is_read  = !is_write;
	}
	return dbg_expr_eval(cond, cond_resolve, &c) != 0;
}

// ----- Public API -----

dbg_tick_t dbg_tick(void) {
	// A watchpoint latched during the previous step. Stop now (the writing
	// instruction has completed; regs.pc is the next instruction).
	if (watch_pending && currentMode != DMODE_STOP) {
		watch_pending  = false;
		last_hit       = pending_hit;
		have_watch_hit = true;
		enter_stop(DBG_BREAK_WATCHPOINT);
		return DBG_TICK_HOLD;
	}

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
		bool hit_user = false;
		for (int i = 0; i < user_bp_count; i++) {
			if (user_bp[i].enabled
			    && hit_bp(regs.pc, regs.k, user_bp[i])
			    && cond_passes(user_bp[i].cond, false, 0, 0, false)) {
				hit_user = true;
				break;
			}
		}
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

	// About to run one instruction; record its start PC so the write hook
	// can attribute any watched write to the right instruction.
	cur_instr_pc   = regs.pc;
	cur_instr_bank = regs.k;
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
	// Snapshot the instruction about to run. dbg_tick refreshes this before
	// every later step, but it runs at the top of the tick, before this resume
	// command is dispatched, so without setting it here a watched read or write
	// by the first stepped instruction would be attributed to the prior
	// (stopped) PC instead of the instruction that made the access.
	cur_instr_pc   = regs.pc;
	cur_instr_bank = regs.k;
	timing_init();
	notify_resume();
}

void dbg_step(void) {
	currentMode           = DMODE_STEP;
	step_start_pc         = regs.pc;
	step_start_pc_bank    = regs.k;
	step_start_pc_x16Bank = dbg_x16_bank(regs.pc, regs.k);
	debugCPUClocks        = clockticks6502;
	cur_instr_pc          = regs.pc;  // attribute the first step (see dbg_continue)
	cur_instr_bank        = regs.k;
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
		cur_instr_pc    = regs.pc;  // attribute the first step (see dbg_continue)
		cur_instr_bank  = regs.k;
		timing_init();
		notify_resume();
	} else {
		dbg_step();
	}
}

void dbg_reset_cpu(void) {
	reset6502(regs.is65c816);
}

// Takes ownership of bp.cond: it is stored on success, freed on any path
// that does not store it (invalid, duplicate, or full).
bool dbg_breakpoint_add(struct breakpoint bp) {
	if (bp.pc < 0) {
		if (bp.cond) dbg_expr_free(bp.cond);
		return false;
	}

	// A breakpoint is always live when added; force the flag so callers using
	// a designated initializer (which leaves enabled at 0) get an enabled one.
	bp.enabled = true;

	// Re-adding at an address that already has a breakpoint replaces the
	// existing entry, so a new `if` clause takes effect. Free the old entry's
	// condition first, then overwrite it in place (its slot, hence its
	// position in the list, is preserved).
	int existing = find_user_bp(bp.pc, bp.bank, bp.x16Bank);
	if (existing >= 0) {
		if (user_bp[existing].cond) dbg_expr_free(user_bp[existing].cond);
		user_bp[existing] = bp;
		return true;
	}

	if (user_bp_count >= DBG_MAX_BREAKPOINTS) {
		if (bp.cond) dbg_expr_free(bp.cond);
		return false; // table full
	}
	user_bp[user_bp_count++] = bp;
	return true;
}

bool dbg_breakpoint_set_enabled(int pc, uint8_t bank, int x16Bank, bool enabled) {
	int i = find_user_bp(pc, bank, x16Bank);
	if (i < 0) {
		return false;
	}
	user_bp[i].enabled = enabled;
	return true;
}

bool dbg_breakpoint_remove(struct breakpoint bp) {
	int i = find_user_bp(bp.pc, bp.bank, bp.x16Bank);
	if (i < 0) {
		return false;
	}
	if (user_bp[i].cond) dbg_expr_free(user_bp[i].cond);
	// Compact the table, preserving insertion order.
	for (int j = i; j < user_bp_count - 1; j++) {
		user_bp[j] = user_bp[j + 1];
	}
	user_bp_count--;
	return true;
}

void dbg_breakpoint_clear_all(void) {
	for (int i = 0; i < user_bp_count; i++) {
		if (user_bp[i].cond) dbg_expr_free(user_bp[i].cond);
	}
	user_bp_count = 0;
}

int dbg_breakpoint_count(void) {
	return user_bp_count;
}

struct breakpoint dbg_breakpoint_get(int idx) {
	if (idx < 0 || idx >= user_bp_count) {
		struct breakpoint none = { -1, 0, -1 };
		return none;
	}
	return user_bp[idx];
}

// Legacy single-slot API, retained for the SDL frontend (F9 toggle and
// breakpoint-status render). SDL only ever manages one breakpoint, and the
// `-debug` and `-debugstdio` frontends are mutually exclusive, so treating
// the table as a single slot here never clobbers stdio's breakpoints.
void dbg_set_breakpoint(struct breakpoint bp) {
	dbg_breakpoint_clear_all();
	// dbg_breakpoint_add frees bp.cond when bp.pc < 0, so this also handles
	// the clear case. The SDL frontend never sets conditions anyway.
	dbg_breakpoint_add(bp);
}

struct breakpoint dbg_get_breakpoint(void) {
	return dbg_breakpoint_get(0);
}

uint32_t dbg_clocks_since_resume(void) {
	return clockticks6502 - debugCPUClocks;
}

// ----- Watchpoints -----

// Takes ownership of cond: stored on success, freed if the request fails.
int dbg_watch_add(int x16Bank, uint16_t start, uint16_t end, bool on_read, bool on_write,
                  struct dbg_expr *cond, const char *cond_src) {
	if ((!on_read && !on_write) || end < start) {
		if (cond) dbg_expr_free(cond);
		return -1;
	}
	for (int i = 0; i < DBG_MAX_WATCHPOINTS; i++) {
		if (!watch_table[i].in_use) {
			watch_table[i].in_use   = true;
			watch_table[i].enabled  = true;
			watch_table[i].on_read  = on_read;
			watch_table[i].on_write = on_write;
			watch_table[i].start    = start;
			watch_table[i].end      = end;
			watch_table[i].x16Bank  = x16Bank;
			watch_table[i].hits     = 0;
			watch_table[i].cond     = cond;
			watch_table[i].cond_src[0] = '\0';
			if (cond_src) {
				strncpy(watch_table[i].cond_src, cond_src, DBG_COND_MAX - 1);
				watch_table[i].cond_src[DBG_COND_MAX - 1] = '\0';
			}
			recompute_watch_armed();
			return i;
		}
	}
	if (cond) dbg_expr_free(cond);
	return -1; // table full
}

bool dbg_watch_remove(int id) {
	if (id < 0 || id >= DBG_MAX_WATCHPOINTS || !watch_table[id].in_use) {
		return false;
	}
	if (watch_table[id].cond) dbg_expr_free(watch_table[id].cond);
	watch_table[id].cond   = NULL;
	watch_table[id].in_use = false;
	recompute_watch_armed();
	return true;
}

void dbg_watch_clear_all(void) {
	for (int i = 0; i < DBG_MAX_WATCHPOINTS; i++) {
		if (watch_table[i].cond) dbg_expr_free(watch_table[i].cond);
		watch_table[i].cond   = NULL;
		watch_table[i].in_use = false;
	}
	dbg_watch_write_armed = 0;
	dbg_watch_read_armed  = 0;
}

bool dbg_watch_set_enabled(int id, bool enabled) {
	if (id < 0 || id >= DBG_MAX_WATCHPOINTS || !watch_table[id].in_use) {
		return false;
	}
	watch_table[id].enabled = enabled;
	recompute_watch_armed();
	return true;
}

int dbg_watch_count(void) {
	int n = 0;
	for (int i = 0; i < DBG_MAX_WATCHPOINTS; i++) {
		if (watch_table[i].in_use) n++;
	}
	return n;
}

bool dbg_watch_get(int id, struct watchpoint *out) {
	if (id < 0 || id >= DBG_MAX_WATCHPOINTS || !watch_table[id].in_use) {
		return false;
	}
	if (out) *out = watch_table[id];
	return true;
}

bool dbg_get_watch_hit(dbg_watch_hit_t *out) {
	if (!have_watch_hit) {
		return false;
	}
	if (out) *out = last_hit;
	return true;
}

// Shared body for the read and write hooks below. is_write selects which
// access flag to match and is recorded on the hit; everything else (the poke
// suppress and pending-hit guards, the range/bank match, the condition, the
// instruction-PC attribution) is identical for both paths.
static void watch_on_access(uint16_t addr, uint8_t value, bool is_write) {
	// Ignore debugger pokes (they route through write6502 too) and ignore
	// further accesses once a hit is pending; the first per instruction wins.
	// Reads have no poke to ignore -- the debugger reads via real_read6502 --
	// but the pending/suppress guards apply to both paths.
	if (watch_suppress || watch_pending) {
		return;
	}
	int eff = dbg_x16_bank(addr, regs.k);
	for (int i = 0; i < DBG_MAX_WATCHPOINTS; i++) {
		struct watchpoint *w = &watch_table[i];
		if (!w->in_use || !w->enabled || (is_write ? !w->on_write : !w->on_read)) {
			continue;
		}
		if (addr < w->start || addr > w->end || w->x16Bank != eff) {
			continue;
		}
		if (!cond_passes(w->cond, true, addr, value, is_write)) {
			continue; // condition false; another watch may still match
		}
		w->hits++;
		pending_hit.id       = i;
		pending_hit.is_write = is_write;
		pending_hit.x16Bank  = eff;
		pending_hit.addr     = addr;
		pending_hit.value    = value;
		pending_hit.pc       = cur_instr_pc;
		pending_hit.pc_bank  = cur_instr_bank;
		watch_pending = true;
		return;
	}
}

void dbg_watch_on_write(uint16_t addr, uint8_t value) {
	watch_on_access(addr, value, true);
}

void dbg_watch_on_read(uint16_t addr, uint8_t value) {
	watch_on_access(addr, value, false);
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
	watch_suppress = true;
	write6502(addr, bank, value);
	watch_suppress = false;
}

void dbg_fill_mem(uint8_t bank, uint16_t addr, uint8_t value, uint16_t len) {
	watch_suppress = true;
	for (uint16_t i = 0; i < len; i++) {
		write6502((uint16_t)(addr + i), bank, value);
	}
	watch_suppress = false;
}

void dbg_fill_mem_buffer(uint32_t addr, int x16bank, uint8_t value, uint32_t count, int incr) {
	if (incr == 0) incr = 1;
	if (x16bank < 0) x16bank = memory_get_ram_bank();
	for (uint32_t i = 0; i < count; i++) {
		addr &= 0xFFFFFF;
		if (addr >= 0xC000 && addr < 0x10000) {
			// ROM range: no-op (matches SDL `f`).
		} else if (addr >= 0xA000 && addr < 0xC000) {
			BRAM[(x16bank << 13) + addr - 0xA000] = value;
		} else if ((addr >> 16) < num_banks) {
			RAM[addr] = value;
		}
		addr = (addr + (uint32_t)incr) & 0xFFFFFF;
	}
}

void dbg_fill_vram_buffer(uint32_t addr, uint8_t value, uint32_t count, int incr) {
	if (incr == 0) incr = 1;
	for (uint32_t i = 0; i < count; i++) {
		video_space_write(addr & 0x1FFFF, value);
		addr = (addr + (uint32_t)incr) & 0x1FFFF;
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
