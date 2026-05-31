// debugger_stdio.c
//
// Stdio frontend for the X16 emulator debugger. Implements the wire protocol
// described in untracked/phases/03-stdio-frontend.md.
//
// Architecture: registers a dbg_frontend_t with debugger_core. The frontend's
// tick() is called once per main-loop iteration. While the emulator is
// running, tick() does a non-blocking peek on stdin. While stopped, tick()
// blocks on stdin until a command arrives (no CPU work happens anyway).
//
// Async events (* BRK / * RES) fire via on_break / on_resume callbacks,
// get buffered, and flush at command boundaries -- never inside a response.

#include "debugger_stdio.h"

#if defined(__EMSCRIPTEN__)
// No stdin in browser; stdio frontend is a build-time no-op.
void debugger_stdio_init(void) {}
void debugger_stdio_shutdown(void) {}

#else
// ===========================================================================
// Frontend implementation. Platform-neutral: all OS-specific terminal and
// stdin handling lives behind debugger_console.h (POSIX and Windows backends).
// ===========================================================================

#include "debugger_core.h"
#include "debugger_term.h"
#include "debugger_console.h"
#include "memory.h"
#include "cpu/fake6502.h"
#include "cpu/registers.h"
#include "version.h"
#include "git_rev.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern struct regs regs;

// ---------------------------------------------------------------------------
// Async event ring: queued by on_break / on_resume, flushed between commands.
// Sized for plenty of headroom; a single tick can realistically queue at most
// 1-2 events.
// ---------------------------------------------------------------------------

#define EVENT_BUF_LINES 16
#define EVENT_BUF_LINE_SZ 64

static char event_buf[EVENT_BUF_LINES][EVENT_BUF_LINE_SZ];
static int  event_count = 0;

// True when the most recent thing emitted on stdout was a prompt and no
// other output has happened since. Used by flush_events to decide whether
// async event lines need to be wrapped in a clear-line / redraw cycle so
// they do not interleave with the user's in-progress input.
static bool at_prompt = false;

static void enqueue_event(const char *fmt, ...) {
	if (event_count >= EVENT_BUF_LINES) {
		return; // drop silently; ring is small but events are rare
	}
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(event_buf[event_count], EVENT_BUF_LINE_SZ, fmt, ap);
	va_end(ap);
	event_count++;
}

static void flush_events(void) {
	if (event_count == 0) return;

	// An async event (BRK / RES) may arrive while the host is sitting at a
	// prompt. The prompt has no trailing newline, so an event emitted right
	// after it would glue onto the prompt line ("x16db > * BRK ...") and a
	// parsing host would not recognise it as an event. On a tty erase the
	// prompt line so the event does not interleave with in-progress input;
	// for a pipe emit a newline so the event lands on its own line. Either
	// way the end-of-tick emit_prompt re-draws the prompt afterward.
	if (at_prompt) {
		if (term_is_tty()) {
			term_clear_line();
		} else {
			fputc('\n', stdout);
		}
		at_prompt = false;
	}
	for (int i = 0; i < event_count; i++) {
		fputs(event_buf[i], stdout);
		fputc('\n', stdout);
	}
	event_count = 0;
	fflush(stdout);
}

// ---------------------------------------------------------------------------
// Frontend callbacks (state-transition events from the core).
// ---------------------------------------------------------------------------

static void stdio_on_break(dbg_break_reason_t r, uint8_t bank, uint16_t pc) {
	if (r == DBG_BREAK_WATCHPOINT) {
		dbg_watch_hit_t h;
		if (dbg_get_watch_hit(&h)) {
			int abank = (h.x16Bank >= 0) ? h.x16Bank : 0;
			enqueue_event("* WP %d %s %02x:%04x=%02x pc=%02x:%04x",
			              h.id, h.is_write ? "w" : "r",
			              abank, h.addr, h.value, h.pc_bank, h.pc);
			return;
		}
	}
	const char *reason;
	switch (r) {
		case DBG_BREAK_USER:          reason = "USER";       break;
		case DBG_BREAK_BREAKPOINT:    reason = "BREAKPOINT"; break;
		case DBG_BREAK_STP_OPCODE:    reason = "STP";        break;
		case DBG_BREAK_STEP_COMPLETE: reason = "STEP";       break;
		default:                      reason = "NONE";       break;
	}
	enqueue_event("* BRK %s %02x %04x", reason, bank, pc);
}

static void stdio_on_resume(void) {
	enqueue_event("* RES");
}

// ---------------------------------------------------------------------------
// Response helpers.
// ---------------------------------------------------------------------------

static void rdy(void) {
	fputs("RDY\n", stdout);
	fflush(stdout);
}

static void err_msg(const char *msg) {
	fprintf(stdout, "ERR %s\n", msg);
	fflush(stdout);
}

static void emit_banner(void) {
	fputs("\nCommander X16 Emulator r" VER " (" VER_NAME ")", stdout);
#ifdef GIT_REV
	fputs(", " GIT_REV, stdout);
#endif
	fputs("\n(C)2019, 2023 Michael Steil et al.\n", stdout);
	fputs("All rights reserved. License: 2-clause BSD\n\n", stdout);
	fflush(stdout);
}

// Header state. Four bracketed lines emitted before each prompt, each
// gated by its own flag (default all on). See `hdr` command.
static bool hdr_cpu_on  = true;   // line 1: instruction + CPU regs/flags
static bool hdr_aux_on  = true;   // line 2: clk + stack top
static bool hdr_view_on = true;   // line 3: view cursor
static bool hdr_bp_on   = true;   // line 4: breakpoint (only when set)

// 8 status bits, MSB first, as '0'/'1'.
static void format_flag_bits(char out[9], uint8_t status) {
	for (int i = 0; i < 8; i++) {
		out[i] = (status & (1u << (7 - i))) ? '1' : '0';
	}
	out[8] = '\0';
}

static void emit_header_cpu(void) {
	dbg_disasm_line_t ln;
	dbg_disasm_line(regs.k, regs.pc, &ln);
	// Bytes column: up to 3 bytes printed as "XX XX XX" (8 chars + trailing
	// space pad to 9). Fourth byte (65C816 long forms) is rare; truncate.
	char bcol[12] = "         "; // 9 spaces + NUL
	for (int j = 0; j < ln.byte_count && j < 3; j++) {
		bcol[j * 3]     = "0123456789abcdef"[ln.bytes[j] >> 4];
		bcol[j * 3 + 1] = "0123456789abcdef"[ln.bytes[j] & 0xF];
	}
	bcol[9] = '\0';
	dbg_regs_snapshot_t r;
	dbg_get_regs(&r);
	char flags[9];
	format_flag_bits(flags, r.status);
	if (r.is_65c816) {
		printf("1: [ %02x:%04x %s %-16s ] [ A=%04x X=%04x Y=%04x S=%04x D=%04x DB=%02x NVMXDIZC=%s E=%u ]\n",
		       regs.k, regs.pc, bcol, ln.text,
		       r.c, r.x16, r.y16, r.sp, r.dp, r.db, flags, r.e ? 1u : 0u);
	} else {
		printf("1: [ %02x:%04x %s %-16s ] [ A=%02x X=%02x Y=%02x S=%04x NV-BDIZC=%s ]\n",
		       regs.k, regs.pc, bcol, ln.text,
		       r.a, r.xl, r.yl, r.sp | 0x100, flags);
	}
}

static void emit_header_aux(void) {
	dbg_stack_entry_t stk[8];
	dbg_get_stack(stk, 8);
	printf("2: [ clk=%u stk=%04x:", dbg_clocks_since_resume(), stk[0].addr);
	for (int i = 0; i < 8; i++) {
		printf("%s%02x", i == 0 ? "" : " ", stk[i].value);
	}
	printf(" ]\n");
}

static void emit_header_view(void) {
	uint8_t  pc_bank;
	uint16_t pc;
	dbg_get_view_pc(&pc_bank, &pc, NULL);
	uint32_t data  = dbg_get_view_data();
	int      vbank = dbg_get_view_x16bank();
	int      mode  = dbg_get_view_mode();
	char vbuf[8];
	if (vbank < 0) strcpy(vbuf, "-");
	else           snprintf(vbuf, sizeof(vbuf), "%02x", vbank);
	printf("3: [ view: pc=%02x:%04x d=%02x:%04x b=%s %s ]\n",
	       pc_bank, pc,
	       (uint8_t)(data >> 16), (uint16_t)(data & 0xFFFF),
	       vbuf,
	       mode == DBG_VIEW_RAM ? "RAM" : "VRAM");
}

static void emit_header_bp(void) {
	int n = dbg_breakpoint_count();
	fputs("4: [ bp=", stdout);
	for (int i = 0; i < n; i++) {
		struct breakpoint bp = dbg_breakpoint_get(i);
		printf("%s%02x:%04x", i ? " " : "", bp.bank, (uint16_t)bp.pc);
	}
	fputs(" ]\n", stdout);
}

static void emit_header(void) {
	if (hdr_cpu_on)  emit_header_cpu();
	if (hdr_aux_on)  emit_header_aux();
	if (hdr_view_on) emit_header_view();
	if (hdr_bp_on && dbg_breakpoint_count() > 0) emit_header_bp();
}

// Per-input prompt. The header (1–4 bracketed lines) precedes it. The
// prompt itself has no trailing newline so a host harness can sync on
// the byte pattern "x16db > " (read-until-suffix-match); a terminal
// user sees a classic shell-style prompt they type their next command
// right after.
static void emit_prompt(void) {
	emit_header();
	fputs("\nx16db > ", stdout);
	// Flush the prompt prefix before term_repaint, which writes the
	// in-progress line buffer via a raw write() that bypasses stdio's
	// buffer; flushing first keeps the prompt and the restored input in
	// order. With an empty buffer term_repaint is a no-op.
	fflush(stdout);
	term_repaint("");
	at_prompt = true;
}

// ---------------------------------------------------------------------------
// Argument parsing.
// ---------------------------------------------------------------------------

static bool parse_hex(const char *s, uint32_t *out, uint32_t max) {
	if (!s || !*s) return false;
	char *end;
	unsigned long v = strtoul(s, &end, 16);
	if (*end != '\0' || v > max) return false;
	*out = (uint32_t)v;
	return true;
}

// Parse "<addr>" or "<bank>:<addr>".  *out_bank = -1 when the bank
// prefix is absent (caller decides whether to leave view_x16bank alone).
static bool parse_bank_addr(const char *s, int *out_bank, uint32_t *out_addr, uint32_t addr_max) {
	if (!s || !*s) return false;
	const char *colon = strchr(s, ':');
	if (colon) {
		char     bbuf[16];
		size_t   blen = (size_t)(colon - s);
		if (blen == 0 || blen >= sizeof(bbuf)) return false;
		memcpy(bbuf, s, blen);
		bbuf[blen] = '\0';
		uint32_t bank;
		if (!parse_hex(bbuf, &bank, 0xff)) return false;
		if (!parse_hex(colon + 1, out_addr, addr_max)) return false;
		*out_bank = (int)bank;
	} else {
		if (!parse_hex(s, out_addr, addr_max)) return false;
		*out_bank = -1;
	}
	return true;
}

// Parse "+[<hex>]" or "-[<hex>]". With no hex, fills the signed offset with
// +default_step / -default_step. Returns true on a well-formed nudge token.
static bool parse_nudge(const char *s, uint32_t default_step, int32_t *out_signed) {
	if (!s || (*s != '+' && *s != '-')) return false;
	bool neg = (*s == '-');
	if (s[1] == '\0') {
		*out_signed = neg ? -(int32_t)default_step : (int32_t)default_step;
		return true;
	}
	uint32_t off;
	if (!parse_hex(s + 1, &off, 0xFFFFFF)) return false;
	*out_signed = neg ? -(int32_t)off : (int32_t)off;
	return true;
}

// ---------------------------------------------------------------------------
// Detach flag (set by `qit`).
// ---------------------------------------------------------------------------

static bool quit_requested = false;

// Forward decl -- defined later, used by both cmd_mem and cmd_vmr.
static void print_hex_ascii_line(uint32_t addr, int addr_width, const uint8_t *bytes, int line);

// ---------------------------------------------------------------------------
// Command implementations.
// ---------------------------------------------------------------------------

static void cmd_brk(int argc, char **argv) {
	(void)argv;
	if (argc != 0) { err_msg("brk takes no args"); return; }
	dbg_break(DBG_BREAK_USER);
	rdy();
}

static void cmd_cnt(int argc, char **argv) {
	(void)argv;
	if (argc != 0) { err_msg("cnt takes no args"); return; }
	dbg_continue();
	rdy();
}

static void cmd_stp(int argc, char **argv) {
	(void)argv;
	if (argc != 0) { err_msg("stp takes no args"); return; }
	dbg_step();
	rdy();
}

static void cmd_sov(int argc, char **argv) {
	(void)argv;
	if (argc != 0) { err_msg("sov takes no args"); return; }
	dbg_step_over();
	rdy();
}

static void cmd_rst(int argc, char **argv) {
	(void)argv;
	if (argc != 0) { err_msg("rst takes no args"); return; }
	dbg_reset_cpu();
	rdy();
}

// Index of an `if` token in argv[from..argc), or argc if there is none.
static int find_if(int argc, char **argv, int from) {
	for (int i = from; i < argc; i++) {
		if (!strcmp(argv[i], "if")) return i;
	}
	return argc;
}

// Join argv[from..argc) into out with single-space separators.
static void join_args(char *out, size_t outsz, int argc, char **argv, int from) {
	size_t len = 0;
	out[0] = '\0';
	for (int i = from; i < argc; i++) {
		size_t l = strlen(argv[i]);
		if (len + (size_t)(i > from ? 1 : 0) + l >= outsz) break;
		if (i > from) out[len++] = ' ';
		memcpy(out + len, argv[i], l);
		len += l;
	}
	out[len] = '\0';
}

// Compile the optional `if` clause at argv[if_at]. Returns NULL (no error)
// when if_at >= argc. On a parse error, reports it via err_msg and sets
// *failed; the source text is written to src_out (size DBG_COND_MAX).
static struct dbg_expr *compile_if(int argc, char **argv, int if_at, char *src_out, bool *failed) {
	*failed = false;
	src_out[0] = '\0';
	if (if_at >= argc) {
		return NULL;
	}
	if (if_at + 1 >= argc) {
		err_msg("if needs a condition");
		*failed = true;
		return NULL;
	}
	join_args(src_out, DBG_COND_MAX, argc, argv, if_at + 1);
	char errbuf[80];
	struct dbg_expr *e = dbg_compile_condition(src_out, errbuf, sizeof(errbuf));
	if (!e) {
		err_msg(errbuf);
		*failed = true;
		return NULL;
	}
	return e;
}

static void cmd_sbp(int argc, char **argv) {
	int if_at = find_if(argc, argv, 0);
	uint32_t bank, addr;
	if (if_at != 2 || !parse_hex(argv[0], &bank, 0xff) || !parse_hex(argv[1], &addr, 0xffff)) {
		err_msg("usage: sbp <bank> <addr> [if <cond>]");
		return;
	}
	char src[DBG_COND_MAX];
	bool failed;
	struct dbg_expr *cond = compile_if(argc, argv, if_at, src, &failed);
	if (failed) return;
	struct breakpoint bp = { .pc = (int)addr, .bank = (uint8_t)bank, .x16Bank = -1, .cond = cond };
	strncpy(bp.cond_src, src, DBG_COND_MAX - 1);
	bp.cond_src[DBG_COND_MAX - 1] = '\0';
	if (!dbg_breakpoint_add(bp)) {  // frees cond on failure
		err_msg("breakpoint table full");
		return;
	}
	rdy();
}

static void cmd_cbp(int argc, char **argv) {
	// `cbp *` clears every breakpoint.
	if (argc == 1 && !strcmp(argv[0], "*")) {
		dbg_breakpoint_clear_all();
		rdy();
		return;
	}
	uint32_t bank, addr;
	if (argc != 2 || !parse_hex(argv[0], &bank, 0xff) || !parse_hex(argv[1], &addr, 0xffff)) {
		err_msg("usage: cbp <bank> <addr> | cbp *");
		return;
	}
	// Remove every breakpoint at this bank:addr (an address may carry more
	// than one entry when set with different x16 banks). Iterate by index,
	// removing matches; the table compacts so re-scan from the same index.
	bool removed = false;
	for (int i = 0; i < dbg_breakpoint_count();) {
		struct breakpoint bp = dbg_breakpoint_get(i);
		if (bp.pc == (int)addr && bp.bank == (uint8_t)bank) {
			dbg_breakpoint_remove(bp);
			removed = true;
		} else {
			i++;
		}
	}
	if (removed) {
		rdy();
	} else {
		err_msg("no such breakpoint");
	}
}

static void cmd_lbp(int argc, char **argv) {
	(void)argv;
	if (argc != 0) { err_msg("lbp takes no args"); return; }
	int n = dbg_breakpoint_count();
	for (int i = 0; i < n; i++) {
		struct breakpoint bp = dbg_breakpoint_get(i);
		printf("%02x: %04x", bp.bank, (uint16_t)bp.pc);
		if (bp.cond_src[0]) printf("  if %s", bp.cond_src);
		printf("\n");
	}
	rdy();
}

// ---------------------------------------------------------------------------
// Watchpoints. Phase A: write watchpoints only; read (`r`/`rw`) and
// conditions (`if`) arrive in later builds, at which point the access
// default becomes `rw`.
// ---------------------------------------------------------------------------

// Parse a non-negative decimal integer (used for watchpoint ids).
static bool parse_dec(const char *s, int *out) {
	if (!s || !*s) return false;
	char *end;
	long v = strtol(s, &end, 10);
	if (*end != '\0' || v < 0) return false;
	*out = (int)v;
	return true;
}

static void cmd_swp(int argc, char **argv) {
	int  i        = 0;
	bool on_read  = false;
	bool on_write = true;  // default is write-only; `r` / `rw` opt into reads
	if (argc > 0 && (!strcmp(argv[0], "r") || !strcmp(argv[0], "w") || !strcmp(argv[0], "rw"))) {
		on_read  = (argv[0][0] == 'r');
		on_write = (strchr(argv[0], 'w') != NULL);
		i = 1;
	}
	int if_at = find_if(argc, argv, i);
	int nspec = if_at - i;  // bank addr [end]
	uint32_t bank, addr, end;
	if (nspec < 2 || nspec > 3
	    || !parse_hex(argv[i], &bank, 0xff)
	    || !parse_hex(argv[i + 1], &addr, 0xffff)) {
		err_msg("usage: swp [r|w|rw] <bank> <addr> [end] [if <cond>]");
		return;
	}
	end = addr;
	if (nspec == 3 && !parse_hex(argv[i + 2], &end, 0xffff)) {
		err_msg("usage: swp [r|w|rw] <bank> <addr> [end] [if <cond>]");
		return;
	}
	if (end < addr) {
		err_msg("end < addr");
		return;
	}
	char src[DBG_COND_MAX];
	bool failed;
	struct dbg_expr *cond = compile_if(argc, argv, if_at, src, &failed);
	if (failed) return;
	// Below the banked window the address is unbanked (x16Bank -1); the
	// bank argument is conventionally 00 there.
	int x16Bank = (addr >= 0xA000) ? (int)bank : -1;
	int id = dbg_watch_add(x16Bank, (uint16_t)addr, (uint16_t)end, on_read, on_write, cond, src);
	if (id < 0) {  // frees cond on failure
		err_msg("watchpoint table full");
		return;
	}
	printf("wp %d set\n", id);
	rdy();
}

static void cmd_lwp(int argc, char **argv) {
	(void)argv;
	if (argc != 0) { err_msg("lwp takes no args"); return; }
	for (int i = 0; i < DBG_MAX_WATCHPOINTS; i++) {
		struct watchpoint w;
		if (!dbg_watch_get(i, &w)) continue;
		int abank = (w.x16Bank >= 0) ? w.x16Bank : 0;
		const char *acc = (w.on_read && w.on_write) ? "rw" : (w.on_read ? "r" : "w");
		printf("%d: %s %02x:%04x", i, acc, abank, w.start);
		if (w.end != w.start) printf("-%04x", w.end);
		printf(" hits=%llu", (unsigned long long)w.hits);
		if (w.cond_src[0]) printf(" if %s", w.cond_src);
		if (!w.enabled) printf(" off");
		printf("\n");
	}
	rdy();
}

static void cmd_cwp(int argc, char **argv) {
	if (argc == 1 && !strcmp(argv[0], "*")) {
		dbg_watch_clear_all();
		rdy();
		return;
	}
	int id;
	if (argc != 1 || !parse_dec(argv[0], &id)) {
		err_msg("usage: cwp <id> | cwp *");
		return;
	}
	if (!dbg_watch_remove(id)) {
		err_msg("no such watchpoint");
		return;
	}
	rdy();
}

static void cmd_wp(int argc, char **argv) {
	int  id;
	bool on;
	if (argc != 2 || !parse_dec(argv[0], &id)) {
		err_msg("usage: wp <id> on|off");
		return;
	}
	if (!strcmp(argv[1], "on"))       on = true;
	else if (!strcmp(argv[1], "off")) on = false;
	else { err_msg("usage: wp <id> on|off"); return; }
	if (!dbg_watch_set_enabled(id, on)) {
		err_msg("no such watchpoint");
		return;
	}
	rdy();
}

static void cmd_reg(int argc, char **argv) {
	(void)argv;
	if (argc != 0) { err_msg("reg takes no args"); return; }
	dbg_regs_snapshot_t r;
	dbg_get_regs(&r);
	// All fields are always present in the output; clients use `mode` to
	// decide which 65C816-specific fields (b, c, db, dp, e, full x/y) are
	// meaningful. In 65C02 mode, x and y carry only their low 8 bits.
	printf("mode=%s pc=%04x a=%02x b=%02x c=%04x x=%04x y=%04x sp=%04x p=%02x k=%02x db=%02x dp=%04x e=%u ram=%02x rom=%02x\n",
	       r.is_65c816 ? "c816" : "c02",
	       r.pc, r.a, r.b, r.c, r.x16, r.y16, r.sp,
	       r.status, r.k, r.db, r.dp, r.e ? 1u : 0u,
	       r.ram_bank, r.rom_bank);
	rdy();
}

static void cmd_srg(int argc, char **argv) {
	uint32_t val;
	if (argc != 2 || !parse_hex(argv[1], &val, 0xffff)) {
		err_msg("usage: srg <name> <hex>");
		return;
	}
	if (!dbg_write_register(argv[0], val)) {
		err_msg("unknown register");
		return;
	}
	rdy();
}

static void cmd_dis(int argc, char **argv) {
	uint32_t bank, addr, count;
	if (argc != 3
	    || !parse_hex(argv[0], &bank,  0xff)
	    || !parse_hex(argv[1], &addr,  0xffff)
	    || !parse_hex(argv[2], &count, 100)) {
		err_msg("usage: dis <bank> <addr> <count>  (count <= 64)");
		return;
	}
	uint16_t cur = (uint16_t)addr;
	for (uint32_t i = 0; i < count; i++) {
		dbg_disasm_line_t line;
		int len = dbg_disasm_line((uint8_t)bank, cur, &line);
		printf("%04x:", cur);
		for (int j = 0; j < line.byte_count; j++) printf(" %02x", line.bytes[j]);
		for (int j = line.byte_count; j < 4; j++) printf("   ");
		printf("  %s\n", line.text);
		cur += (uint16_t)len;
	}
	rdy();
}

static void cmd_vmr(int argc, char **argv) {
	uint32_t addr, count;
	if (argc != 2
	    || !parse_hex(argv[0], &addr,  0x1ffff)
	    || !parse_hex(argv[1], &count, 0x1000)) {
		err_msg("usage: vmr <addr> <count>  (count <= 1000)");
		return;
	}
	uint32_t cur = addr;
	uint8_t  bytes[16];
	while (count > 0) {
		int line = count > 16 ? 16 : (int)count;
		for (int i = 0; i < line; i++) {
			bytes[i] = dbg_read_vram((cur + i) & 0x1ffff);
		}
		print_hex_ascii_line(cur, 5, bytes, line);
		cur = (cur + line) & 0x1ffff;
		count -= line;
	}
	rdy();
}

static void cmd_vmw(int argc, char **argv) {
	if (argc < 2) { err_msg("usage: vmw <addr> <hex>..."); return; }
	uint32_t addr;
	if (!parse_hex(argv[0], &addr, 0x1ffff)) {
		err_msg("usage: vmw <addr> <hex>...");
		return;
	}
	for (int i = 1; i < argc; i++) {
		uint32_t v;
		if (!parse_hex(argv[i], &v, 0xff)) { err_msg("bad byte"); return; }
		dbg_write_vram((addr + (uint32_t)(i - 1)) & 0x1ffff, (uint8_t)v);
	}
	rdy();
}

static void cmd_stk(int argc, char **argv) {
	uint32_t count = 16;
	if (argc > 1)                          { err_msg("usage: stk [<count>]"); return; }
	if (argc == 1 && !parse_hex(argv[0], &count, 40)) {
		err_msg("usage: stk [<count>]  (count <= 40)");
		return;
	}
	dbg_stack_entry_t entries[64];
	dbg_get_stack(entries, (int)count);
	for (uint32_t i = 0; i < count; i++) {
		printf("%04x: %02x\n", entries[i].addr, entries[i].value);
	}
	rdy();
}

static void cmd_zpr(int argc, char **argv) {
	(void)argv;
	if (argc != 0) { err_msg("zpr takes no args"); return; }
	uint16_t pairs[16];
	dbg_get_zp_pairs(pairs);
	for (int i = 0; i < 16; i++) {
		printf("R%-2d %04x\n", i, pairs[i]);
	}
	rdy();
}

static void cmd_vrg(int argc, char **argv) {
	(void)argv;
	if (argc != 0) { err_msg("vrg takes no args"); return; }
	dbg_vera_snapshot_t v;
	dbg_get_vera(&v);
	printf("addr0=%05x addr1=%05x data0=%02x data1=%02x ctrl=%02x video=%02x hscale=%02x vscale=%02x fxctl=%02x fxmul=%02x cache=%02x%02x%02x%02x accum=%08x\n",
	       v.addr0, v.addr1, v.data0, v.data1, v.ctrl, v.video,
	       v.hscale, v.vscale, v.fxctl, v.fxmul,
	       v.cache[0], v.cache[1], v.cache[2], v.cache[3], v.accum);
	rdy();
}

static void cmd_fil(int argc, char **argv) {
	uint32_t bank, addr, value, count = 1;
	if (argc < 3 || argc > 4
	    || !parse_hex(argv[0], &bank,  0xff)
	    || !parse_hex(argv[1], &addr,  0xffff)
	    || !parse_hex(argv[2], &value, 0xff)) {
		err_msg("usage: fil <bank> <addr> <value> [<count>]");
		return;
	}
	if (argc == 4 && !parse_hex(argv[3], &count, 0xffff)) {
		err_msg("count must be hex");
		return;
	}
	dbg_fill_mem((uint8_t)bank, (uint16_t)addr, (uint8_t)value, (uint16_t)count);
	rdy();
}

static void cmd_clk(int argc, char **argv) {
	(void)argv;
	if (argc != 0) { err_msg("clk takes no args"); return; }
	printf("clocks=%u\n", dbg_clocks_since_resume());
	rdy();
}

static void cmd_find(int argc, char **argv) {
	if (argc < 4) {
		err_msg("usage: find <bank> <start> <len> <byte>... (pattern <= 16 bytes)");
		return;
	}
	uint32_t bank, start, len;
	if (!parse_hex(argv[0], &bank,  0xff)
	    || !parse_hex(argv[1], &start, 0xffff)
	    || !parse_hex(argv[2], &len,  0xffff)) {
		err_msg("usage: find <bank> <start> <len> <byte>...");
		return;
	}
	int pat_len = argc - 3;
	if (pat_len > 16) { err_msg("pattern too long (max 16 bytes)"); return; }
	uint8_t pattern[16];
	for (int i = 0; i < pat_len; i++) {
		uint32_t b;
		if (!parse_hex(argv[3 + i], &b, 0xff)) { err_msg("bad pattern byte"); return; }
		pattern[i] = (uint8_t)b;
	}
	for (uint32_t off = 0; (int32_t)(len - off) >= pat_len; off++) {
		bool match = true;
		for (int i = 0; i < pat_len; i++) {
			if (dbg_read_mem((uint8_t)bank, (uint16_t)(start + off + i), -1) != pattern[i]) {
				match = false;
				break;
			}
		}
		if (match) {
			printf("%04x\n", (uint16_t)(start + off));
		}
	}
	rdy();
}

// Defined in main.c. cmd_bail bumps this and the main loop returns it.
extern int exit_code;

static void cmd_bail(int argc, char **argv) {
	(void)argv;
	if (argc != 0) { err_msg("bail takes no args"); return; }
	exit_code      = 1;
	quit_requested = true;
	rdy();
}

// Print one hex-dump line: "<addr>: hh hh ... hh  ASCII...". `addr_width` is
// 4 (CPU bus) or 5 (VRAM). The ASCII column shows printable ASCII (0x20-0x7e)
// verbatim and substitutes '.' for anything else. Pads the hex column so the
// ASCII column aligns even for short final lines.
static void print_hex_ascii_line(uint32_t addr, int addr_width, const uint8_t *bytes, int line) {
	printf("%0*x:", addr_width, addr);
	for (int i = 0; i < line; i++) printf(" %02x", bytes[i]);
	for (int i = line; i < 16; i++) printf("   ");
	printf("  ");
	for (int i = 0; i < line; i++) {
		uint8_t b = bytes[i];
		putchar((b >= 0x20 && b <= 0x7e) ? (char)b : '.');
	}
	printf("\n");
}

static void cmd_mem(int argc, char **argv) {
	uint32_t bank, addr, count;
	if (argc != 3
	    || !parse_hex(argv[0], &bank, 0xff)
	    || !parse_hex(argv[1], &addr, 0xffff)
	    || !parse_hex(argv[2], &count, 0x1000)) {
		err_msg("usage: mem <bank> <addr> <count>  (count <= 1000)");
		return;
	}
	uint16_t cur = (uint16_t)addr;
	uint8_t  bytes[16];
	while (count > 0) {
		int line = count > 16 ? 16 : (int)count;
		for (int i = 0; i < line; i++) {
			bytes[i] = dbg_read_mem((uint8_t)bank, (uint16_t)(cur + i), -1);
		}
		print_hex_ascii_line(cur, 4, bytes, line);
		cur   += line;
		count -= line;
	}
	rdy();
}

static void cmd_wmm(int argc, char **argv) {
	if (argc < 3) { err_msg("usage: wmm <bank> <addr> <hex>..."); return; }
	uint32_t bank, addr;
	if (!parse_hex(argv[0], &bank, 0xff) || !parse_hex(argv[1], &addr, 0xffff)) {
		err_msg("usage: wmm <bank> <addr> <hex>...");
		return;
	}
	for (int i = 2; i < argc; i++) {
		uint32_t v;
		if (!parse_hex(argv[i], &v, 0xff)) { err_msg("bad byte"); return; }
		dbg_write_mem((uint8_t)bank, (uint16_t)(addr + i - 2), (uint8_t)v);
	}
	rdy();
}

static void cmd_mod(int argc, char **argv) {
	(void)argv;
	if (argc != 0) { err_msg("mod takes no args"); return; }
	const char *m;
	int mode = dbg_get_mode();
	switch (mode) {
		case DMODE_RUN:  m = "run";  break;
		case DMODE_STEP: m = "step"; break;
		case DMODE_STOP: m = "stop"; break;
		default:         m = "?";    break;
	}
	uint8_t  bank;
	uint16_t pc;
	if (mode == DMODE_STOP) {
		dbg_get_pc(&bank, &pc);
	} else {
		dbg_regs_snapshot_t r;
		dbg_get_regs(&r);
		bank = r.k;
		pc   = r.pc;
	}
	printf("mode=%s pc=%02x:%04x\n", m, bank, pc);
	rdy();
}

static void cmd_ver(int argc, char **argv) {
	(void)argv;
	if (argc != 0) { err_msg("ver takes no args"); return; }
	printf("proto=1\n");
	rdy();
}

static void cmd_qit(int argc, char **argv) {
	(void)argv;
	if (argc != 0) { err_msg("qit takes no args"); return; }
	quit_requested = true;
	rdy();
}

// ---------------------------------------------------------------------------
// SDL-equivalent stateful commands.
//
// These mutate the shared view cursor (debugger_core's view_pc /
// view_data / view_x16bank / view_mode) and dump using SDL-style panel
// defaults, so a user's mental model carries over between the SDL TUI
// and the stdio REPL: same commands, same effects, same cognitive shape.
//
// The stateless explicit-args commands (mem / dis / vmr / wmm / fil /
// srg / sbp / cbp / lbp) coexist; harnesses prefer those because they
// don't touch the cursor.
// ---------------------------------------------------------------------------

// One row of the RAM data panel: addr + 16 hex bytes + ASCII column.
static void dump_ram_row(uint32_t row_addr_24, int x16bank) {
	uint8_t bytes[16];
	for (int i = 0; i < 16; i++) {
		uint32_t a = (row_addr_24 + i) & 0xFFFFFF;
		bytes[i] = dbg_read_mem((uint8_t)(a >> 16), (uint16_t)(a & 0xFFFF), x16bank);
	}
	// print_hex_ascii_line takes a 16-bit addr; pass the low word with
	// the high byte implicit in the dump's starting bank context.
	print_hex_ascii_line(row_addr_24 & 0xFFFF, 4, bytes, 16);
}

static void dump_view_data_ram(void) {
	uint32_t base    = dbg_get_view_data();
	int      x16bank = dbg_get_view_x16bank();
	for (int row = 0; row < 16; row++) {
		dump_ram_row((base + row * 16) & 0xFFFFFF, x16bank);
	}
}

static void dump_view_data_vram(void) {
	uint32_t base = dbg_get_view_data() & 0x1FFFF;
	uint8_t  bytes[16];
	for (int row = 0; row < 16; row++) {
		uint32_t row_addr = (base + row * 16) & 0x1FFFF;
		for (int i = 0; i < 16; i++) {
			bytes[i] = dbg_read_vram((row_addr + i) & 0x1FFFF);
		}
		print_hex_ascii_line(row_addr, 5, bytes, 16);
	}
}

static void dump_view_disasm(int count) {
	uint8_t  bank;
	uint16_t pc;
	dbg_get_view_pc(&bank, &pc, NULL);
	uint16_t cur = pc;
	for (int i = 0; i < count; i++) {
		dbg_disasm_line_t line;
		int len = dbg_disasm_line(bank, cur, &line);
		printf("%02x:%04x:", bank, cur);
		for (int j = 0; j < line.byte_count; j++) printf(" %02x", line.bytes[j]);
		for (int j = line.byte_count; j < 4; j++) printf("   ");
		printf("  %s\n", line.text);
		cur += (uint16_t)len;
	}
}

// Re-compute x16Bank for a given PC, matching debugger.c's getCurrentBank().
static int view_pc_x16bank_for(uint16_t pc, uint8_t bank) {
	if (pc >= 0xA000 && bank == 0) {
		return pc < 0xC000 ? memory_get_ram_bank() : memory_get_rom_bank();
	}
	return -1;
}

static void cmd_m(int argc, char **argv) {
	if (argc > 1) { err_msg("usage: m [<bank>:<addr> | +[<off>] | -[<off>]]"); return; }
	if (argc == 1) {
		int32_t delta;
		if (parse_nudge(argv[0], 0x100, &delta)) {
			uint32_t cur = dbg_get_view_data();
			dbg_set_view_data(((uint32_t)((int32_t)cur + delta)) & 0xFFFFFF);
			dbg_set_view_mode(DBG_VIEW_RAM);
		} else {
			int      bank;
			uint32_t addr;
			if (!parse_bank_addr(argv[0], &bank, &addr, 0xFFFFFF)) {
				err_msg("usage: m [<bank>:<addr> | +[<off>] | -[<off>]]");
				return;
			}
			if (bank >= 0) dbg_set_view_x16bank(bank);
			dbg_set_view_data(addr);
			dbg_set_view_mode(DBG_VIEW_RAM);
		}
	} else {
		dbg_set_view_mode(DBG_VIEW_RAM);
	}
	dump_view_data_ram();
	rdy();
}

static void cmd_d(int argc, char **argv) {
	if (argc > 1) { err_msg("usage: d [<bank>:<addr> | +[<off>] | -[<off>]]"); return; }
	if (argc == 1) {
		int32_t delta;
		if (parse_nudge(argv[0], 0x10, &delta)) {
			uint8_t  cur_bank;
			uint16_t cur_pc;
			int      cur_x16;
			dbg_get_view_pc(&cur_bank, &cur_pc, &cur_x16);
			uint16_t new_pc = (uint16_t)((int32_t)cur_pc + delta);
			int      x16    = view_pc_x16bank_for(new_pc, cur_bank);
			dbg_set_view_pc(cur_bank, new_pc, x16);
		} else {
			int      bank;
			uint32_t addr;
			if (!parse_bank_addr(argv[0], &bank, &addr, 0xFFFF)) {
				err_msg("usage: d [<bank>:<addr> | +[<off>] | -[<off>]]");
				return;
			}
			uint8_t  cur_bank;
			dbg_get_view_pc(&cur_bank, NULL, NULL);
			uint8_t new_bank = (bank >= 0) ? (uint8_t)bank : cur_bank;
			int x16bank = (bank >= 0 && (uint16_t)addr >= 0xA000) ? bank
			              : view_pc_x16bank_for((uint16_t)addr, new_bank);
			dbg_set_view_pc(new_bank, (uint16_t)addr, x16bank);
		}
	}
	dump_view_disasm(16);
	rdy();
}

static void cmd_v(int argc, char **argv) {
	if (argc > 1) { err_msg("usage: v [<addr> | +[<off>] | -[<off>]]"); return; }
	if (argc == 1) {
		int32_t delta;
		if (parse_nudge(argv[0], 0x200, &delta)) {
			uint32_t cur = dbg_get_view_data();
			dbg_set_view_data(((uint32_t)((int32_t)cur + delta)) & 0x1FFFF);
		} else {
			uint32_t addr;
			if (!parse_hex(argv[0], &addr, 0x1FFFF)) {
				err_msg("usage: v [<addr> | +[<off>] | -[<off>]]  (addr <= 1ffff)");
				return;
			}
			dbg_set_view_data(addr);
		}
	}
	dbg_set_view_mode(DBG_VIEW_VRAM);
	dump_view_data_vram();
	rdy();
}

static void cmd_b(int argc, char **argv) {
	if (argc < 1 || argc > 2) {
		err_msg("usage: b ram|rom|view <bank>  |  b view +|-|follow");
		return;
	}
	if (!strcmp(argv[0], "ram") || !strcmp(argv[0], "rom")) {
		uint32_t bank;
		if (argc != 2 || !parse_hex(argv[1], &bank, 0xff)) {
			err_msg("usage: b ram|rom <bank>");
			return;
		}
		if (!strcmp(argv[0], "ram")) memory_set_ram_bank((uint8_t)bank);
		else                          memory_set_rom_bank((uint8_t)bank);
		rdy();
		return;
	}
	if (!strcmp(argv[0], "view")) {
		if (argc != 2) {
			err_msg("usage: b view <bank>|+|-|follow");
			return;
		}
		if (!strcmp(argv[1], "follow")) {
			dbg_set_view_x16bank(-1);
			rdy();
			return;
		}
		if (!strcmp(argv[1], "+") || !strcmp(argv[1], "-")) {
			int cur = dbg_get_view_x16bank();
			if (cur < 0) {
				// "follow CPU" -> start from the CPU's currently-selected bank
				// so the nudge moves one off from where the user actually sees.
				cur = (int)memory_get_ram_bank();
			}
			int step = (argv[1][0] == '+') ? 1 : -1;
			dbg_set_view_x16bank((cur + step) & 0xff);
			rdy();
			return;
		}
		uint32_t bank;
		if (!parse_hex(argv[1], &bank, 0xff)) {
			err_msg("usage: b view <bank>|+|-|follow");
			return;
		}
		dbg_set_view_x16bank((int)bank);
		rdy();
		return;
	}
	err_msg("usage: b ram|rom|view ...");
}

static void cmd_r(int argc, char **argv) {
	uint32_t val;
	if (argc != 2 || !parse_hex(argv[1], &val, 0xffff)) {
		err_msg("usage: r <name> <hex>");
		return;
	}
	if (!dbg_write_register(argv[0], val)) {
		err_msg("unknown register");
		return;
	}
	rdy();
}

static void cmd_f(int argc, char **argv) {
	uint32_t addr, value;
	uint32_t count = 1, incr = 1;
	if (argc < 2 || argc > 4
	    || !parse_hex(argv[0], &addr,  0xFFFFFF)
	    || !parse_hex(argv[1], &value, 0xff)) {
		err_msg("usage: f <addr> <val> [<count>] [<incr>]");
		return;
	}
	if (argc >= 3 && !parse_hex(argv[2], &count, 0x1000000)) {
		err_msg("count must be hex");
		return;
	}
	if (argc == 4 && !parse_hex(argv[3], &incr, 0xff)) {
		err_msg("incr must be hex");
		return;
	}
	if (dbg_get_view_mode() == DBG_VIEW_RAM) {
		dbg_fill_mem_buffer(addr, dbg_get_view_x16bank(), (uint8_t)value, count, (int)incr);
	} else {
		dbg_fill_vram_buffer(addr, (uint8_t)value, count, (int)incr);
	}
	rdy();
}

static void cmd_home(int argc, char **argv) {
	(void)argv;
	if (argc != 0) { err_msg("home takes no args"); return; }
	dbg_set_view_pc(regs.k, regs.pc, view_pc_x16bank_for(regs.pc, regs.k));
	dump_view_disasm(16);
	rdy();
}

static void cmd_tb(int argc, char **argv) {
	(void)argv;
	if (argc != 0) { err_msg("tb takes no args"); return; }
	uint8_t  bank;
	uint16_t pc;
	int      x16bank;
	dbg_get_view_pc(&bank, &pc, &x16bank);
	struct breakpoint bp = { .pc = (int)pc, .bank = bank, .x16Bank = x16bank };
	if (dbg_breakpoint_remove(bp)) {
		printf("* BP CLEAR %02x:%04x\n", bank, pc);
	} else if (dbg_breakpoint_add(bp)) {
		printf("* BP SET %02x:%04x\n", bank, pc);
	} else {
		err_msg("breakpoint table full");
		return;
	}
	rdy();
}

static bool *hdr_flag_for_name(const char *name) {
	if (!strcmp(name, "cpu")  || !strcmp(name, "1")) return &hdr_cpu_on;
	if (!strcmp(name, "aux")  || !strcmp(name, "2")) return &hdr_aux_on;
	if (!strcmp(name, "view") || !strcmp(name, "3")) return &hdr_view_on;
	if (!strcmp(name, "bp")   || !strcmp(name, "4")) return &hdr_bp_on;
	return NULL;
}

static void cmd_hdr(int argc, char **argv) {
	if (argc == 0) {
		// Status report. Each line is a copy-paste-valid setter command,
		// so the user can re-apply state by piping the output back in.
		printf("hdr cpu  %s\n", hdr_cpu_on  ? "on" : "off");
		printf("hdr aux  %s\n", hdr_aux_on  ? "on" : "off");
		printf("hdr view %s\n", hdr_view_on ? "on" : "off");
		printf("hdr bp   %s\n", hdr_bp_on   ? "on" : "off");
		rdy();
		return;
	}
	if (argc == 1) {
		bool on;
		if      (!strcmp(argv[0], "on"))  on = true;
		else if (!strcmp(argv[0], "off")) on = false;
		else { err_msg("usage: hdr [on|off | <name>|<num> on|off]"); return; }
		hdr_cpu_on = hdr_aux_on = hdr_view_on = hdr_bp_on = on;
		rdy();
		return;
	}
	if (argc == 2) {
		bool *flag = hdr_flag_for_name(argv[0]);
		if (!flag) { err_msg("unknown header (use cpu|aux|view|bp or 1-4)"); return; }
		if      (!strcmp(argv[1], "on"))  *flag = true;
		else if (!strcmp(argv[1], "off")) *flag = false;
		else { err_msg("usage: hdr <name>|<num> on|off"); return; }
		rdy();
		return;
	}
	err_msg("usage: hdr [on|off | <name>|<num> on|off]");
}

static void cmd_st(int argc, char **argv) {
	(void)argv;
	if (argc != 0) { err_msg("st takes no args"); return; }
	int mode = dbg_get_mode();
	const char *modestr = mode == DMODE_RUN  ? "run"
	                    : mode == DMODE_STEP ? "step"
	                    :                      "stop";
	uint8_t  pc_bank;
	uint16_t pc;
	int      pc_x16bank;
	dbg_get_view_pc(&pc_bank, &pc, &pc_x16bank);
	uint32_t data = dbg_get_view_data();
	int      vbank = dbg_get_view_x16bank();
	int      vmode = dbg_get_view_mode();
	dbg_regs_snapshot_t r;
	dbg_get_regs(&r);
	printf("mode      %s\n", modestr);
	printf("view_pc   %02x:%04x  x16bank=%d\n", pc_bank, pc, pc_x16bank);
	printf("view_data %02x:%04x\n", (uint8_t)(data >> 16), (uint16_t)(data & 0xFFFF));
	printf("view_bank %d\n", vbank);
	printf("view_mode %s\n", vmode == DBG_VIEW_RAM ? "RAM" : "VRAM");
	printf("regs.pc   %02x:%04x\n", r.k, r.pc);
	printf("clk       %u\n", dbg_clocks_since_resume());
	int nbp = dbg_breakpoint_count();
	if (nbp == 0) {
		printf("bp        (none)\n");
	} else {
		for (int i = 0; i < nbp; i++) {
			struct breakpoint bp = dbg_breakpoint_get(i);
			printf("bp        %02x:%04x  x16bank=%d\n", bp.bank, (uint16_t)bp.pc, bp.x16Bank);
		}
	}
	rdy();
}

static void cmd_hlp(int argc, char **argv) {
	(void)argv;
	if (argc != 0) { err_msg("hlp takes no args"); return; }
	// One line per command. The protocol's RDY terminator follows.
	puts("execution control:");
	puts("  brk                                 force-break into the debugger");
	puts("  cnt | c                             continue (resume CPU)");
	puts("  stp | s                             single step one instruction");
	puts("  sov | n                             step over JSR/JSL/JML (else single-step)");
	puts("  rst                                 reset CPU (not the whole machine)");
	puts("SDL-equivalent (operate on the shared view cursor):");
	puts("  m [<bank>:<addr> | +[<off>] | -[<off>]]  data cursor + dump 16 rows RAM");
	puts("                                           bare +/- nudges by 0x100");
	puts("  d [<bank>:<addr> | +[<off>] | -[<off>]]  disasm cursor + 16 instructions");
	puts("                                           bare +/- nudges by 0x10");
	puts("  v [<addr> | +[<off>] | -[<off>]]         data cursor + dump 16 rows VRAM");
	puts("                                           bare +/- nudges by 0x200");
	puts("  b ram|rom <bank>                         set CPU's RAM/ROM bank register");
	puts("  b view <bank>|+|-|follow                 view-bank override (or follow CPU)");
	puts("  r <name> <hex>                           set register (pc/a/b/c/x/y/sp/p/k/db/dp/e)");
	puts("  f <addr> <val> [<count>] [<incr>]        fill RAM or VRAM (bypasses I/O)");
	puts("  home                                     view_pc := regs.pc; dump disasm");
	puts("  tb                                       toggle breakpoint at view_pc");
	puts("breakpoints (up to 16, explicit-args):");
	puts("  sbp <bank> <addr> [if <cond>]       add a user breakpoint");
	puts("  cbp <bank> <addr> | cbp *           clear a breakpoint, or all");
	puts("  lbp                                 list breakpoints");
	puts("watchpoints (up to 16, write-only for now):");
	puts("  swp [r|w|rw] <bank> <addr> [end] [if <cond>]  add a watchpoint (default w)");
	puts("  cwp <id> | cwp *                    clear a watchpoint, or all");
	puts("  lwp                                 list watchpoints");
	puts("  wp <id> on|off                      enable/disable a watchpoint");
	puts("memory (explicit-args, stateless):");
	puts("  mem <bank> <addr> <count>           read memory (count <= 0x1000)");
	puts("  wmm <bank> <addr> <hex>...          write memory bytes");
	puts("  fil <bank> <addr> <val> [<count>]   fill memory through write6502");
	puts("  find <bank> <start> <len> <byte>... find byte pattern in range");
	puts("  vmr <addr> <count>                  read VRAM (count <= 0x1000)");
	puts("  vmw <addr> <hex>...                 write VRAM bytes");
	puts("  dis <bank> <addr> <count>           disassemble N instructions");
	puts("inspection:");
	puts("  reg                                 dump CPU registers");
	puts("  srg <name> <hex>                    set register (explicit form of `r`)");
	puts("  stk [<count>]                       stack top (default 16, max 0x40)");
	puts("  zpr                                 direct-page R0..R15 register pairs");
	puts("  vrg                                 VERA state snapshot");
	puts("  clk                                 clocks since last resume");
	puts("header / status (header lines emit before each prompt):");
	puts("  hdr                                 show per-line on/off status");
	puts("  hdr on | off                        toggle all four header lines");
	puts("  hdr cpu|aux|view|bp on|off          toggle a specific header line");
	puts("  hdr 1|2|3|4 on|off                  same, by line number");
	puts("  st                                  full state dump (mode, view, regs, clk, bp)");
	puts("session:");
	puts("  mod                                 current mode + PC");
	puts("  ver                                 protocol version");
	puts("  hlp                                 this help (aliases: h, ?, help)");
	puts("  quit                                detach, exit 0  (alias: qit)");
	puts("  bail                                detach, exit 1  (for harness assertions)");
	rdy();
}

// ---------------------------------------------------------------------------
// Command dispatch table.
// ---------------------------------------------------------------------------

typedef void (*cmd_fn)(int argc, char **argv);

static const struct {
	const char *name;
	cmd_fn      fn;
} commands[] = {
	// execution control (single-letter shell-style shortcuts)
	{"brk",  cmd_brk},
	{"cnt",  cmd_cnt},   {"c", cmd_cnt},
	{"stp",  cmd_stp},   {"s", cmd_stp},
	{"sov",  cmd_sov},   {"n", cmd_sov},   // n = "next"
	{"rst",  cmd_rst},
	// SDL-equivalent stateful commands (operate on the shared view cursor)
	{"m",    cmd_m},
	{"d",    cmd_d},
	{"v",    cmd_v},
	{"b",    cmd_b},
	{"r",    cmd_r},
	{"f",    cmd_f},
	{"home", cmd_home},
	{"tb",   cmd_tb},
	// breakpoints (explicit-args, stateless)
	{"sbp",  cmd_sbp},
	{"cbp",  cmd_cbp},
	{"lbp",  cmd_lbp},
	// watchpoints (data breakpoints)
	{"swp",  cmd_swp},
	{"cwp",  cmd_cwp},
	{"lwp",  cmd_lwp},
	{"wp",   cmd_wp},
	// registers (stateless)
	{"reg",  cmd_reg},
	{"srg",  cmd_srg},
	// memory (CPU bus, stateless)
	{"mem",  cmd_mem},
	{"wmm",  cmd_wmm},
	{"fil",  cmd_fil},
	{"find", cmd_find},
	// memory (VRAM, stateless)
	{"vmr",  cmd_vmr},
	{"vmw",  cmd_vmw},
	// disassembly (stateless)
	{"dis",  cmd_dis},
	// snapshot panels (parity with SDL)
	{"stk",  cmd_stk},
	{"zpr",  cmd_zpr},
	{"vrg",  cmd_vrg},
	{"clk",  cmd_clk},
	// header / status
	{"hdr",  cmd_hdr},
	{"st",   cmd_st},
	// session
	{"mod",  cmd_mod},
	{"ver",  cmd_ver},
	{"h",    cmd_hlp},   // help aliases
	{"hlp",  cmd_hlp},
	{"?",    cmd_hlp},
	{"help", cmd_hlp},
	{"qit",  cmd_qit},   // quit aliases
	{"quit", cmd_qit},
	{"bail", cmd_bail},
	{NULL,   NULL},
};

static void dispatch_line(char *line) {
	at_prompt = false;
	// Strip trailing CR/LF.
	size_t len = strlen(line);
	while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
		line[--len] = '\0';
	}
	// Tokenize on whitespace. Single-threaded frontend, so strtok is fine.
	char *argv[16];
	int argc = 0;
	char *verb = strtok(line, " \t");
	if (!verb) return; // empty / whitespace-only line: ignore silently per spec
	for (char *p = verb; *p; p++) *p = (char)tolower((unsigned char)*p);
	char *tok;
	while ((tok = strtok(NULL, " \t")) != NULL && argc < 16) {
		argv[argc++] = tok;
	}
	for (int i = 0; commands[i].name; i++) {
		if (!strcmp(commands[i].name, verb)) {
			commands[i].fn(argc, argv);
			return;
		}
	}
	err_msg("unknown command");
}

// ---------------------------------------------------------------------------
// Non-blocking stdin reader.
//
// Reads raw bytes (via the platform console backend) into a private
// accumulator and extracts complete lines from it. Using fgets here would be
// a trap: it pulls bytes into libc's stdin buffer in chunks that the backend's
// readiness wait can't see, so a queued line could sit in libc unnoticed.
// Going through the backend's raw read keeps everything visible.
// ---------------------------------------------------------------------------

#define LINE_ACCUM_SZ 8192

static char   line_accum[LINE_ACCUM_SZ];
static size_t line_accum_len = 0;
static bool   stdin_eof      = false;

// Pull whatever is available right now into line_accum. Returns -1 on EOF.
//
// Two paths: in tty mode, feed each byte through the debugger_term line
// editor (which echoes printable chars, handles backspace and Enter, and
// absorbs escape sequences), and splice each completed line into line_accum
// with a trailing '\n' so extract_line picks it up unchanged. In pipe mode,
// read in bulk straight into line_accum -- the protocol path the testbench
// and scripts depend on.
static int drain_stdin(void) {
	if (term_is_tty()) {
		for (;;) {
			uint8_t buf[256];
			int n = dbg_console_read(buf, sizeof(buf));
			if (n < 0) { stdin_eof = true; return -1; }
			if (n == 0) return 0;  // nothing available right now
			for (int i = 0; i < n; i++) {
				if (term_feed_byte(buf[i])) {
					char   tmp[LINE_ACCUM_SZ];
					size_t len = term_take_line(tmp, sizeof(tmp));
					if (line_accum_len + len + 1 >= LINE_ACCUM_SZ) {
						// No room for the completed line + its '\n'; drop it
						// rather than half-splice. In practice LINE_ACCUM_SZ
						// is far larger than any plausible command.
						continue;
					}
					memcpy(line_accum + line_accum_len, tmp, len);
					line_accum_len += len;
					line_accum[line_accum_len++] = '\n';
				}
			}
			if (line_accum_len >= LINE_ACCUM_SZ - 1) return 0;
		}
	}

	while (line_accum_len < LINE_ACCUM_SZ - 1) {
		int n = dbg_console_read(line_accum + line_accum_len,
		                         LINE_ACCUM_SZ - 1 - line_accum_len);
		if (n < 0) { stdin_eof = true; return -1; }
		if (n == 0) return 0;  // nothing available right now
		line_accum_len += (size_t)n;
	}
	return 0;
}

// Extract one complete line (terminated by '\n') into `out`. Returns true on
// success; false if the accumulator has no full line yet.
static bool extract_line(char *out, size_t outsz) {
	char *nl = memchr(line_accum, '\n', line_accum_len);
	if (!nl) return false;
	size_t llen = (size_t)(nl - line_accum);
	if (llen >= outsz) llen = outsz - 1;
	memcpy(out, line_accum, llen);
	out[llen] = '\0';
	size_t consumed = (size_t)(nl - line_accum) + 1;
	memmove(line_accum, line_accum + consumed, line_accum_len - consumed);
	line_accum_len -= consumed;
	return true;
}

// ---------------------------------------------------------------------------
// Main-loop hook (called once per iteration via the vtable).
// ---------------------------------------------------------------------------

static int stdio_tick(void) {
	// 1. Advance state machine (BP / step-complete detection fires callbacks).
	dbg_tick();
	bool produced_output = (event_count > 0);
	flush_events();

	// 2. Drain stdin (sets stdin_eof on EOF) and dispatch AT MOST ONE
	// complete line per tick. Doing more would prevent the CPU from
	// stepping between commands -- a stp followed by reg would show
	// pre-step state, and the * BRK STEP event would never fire.
	// One-per-tick is fast in practice: buffered lines process back-to-
	// back across main-loop iterations (tens of microseconds apart).
	(void)drain_stdin();

	char line[4096];
	if (extract_line(line, sizeof(line))) {
		dispatch_line(line);
		if (event_count > 0) produced_output = true;
		flush_events();
		produced_output = true; // a command response is always output
	} else if (stdin_eof) {
		// No more complete lines. If there's a trailing partial line
		// (writer closed without a final \n), process it now then exit;
		// otherwise just exit.
		if (line_accum_len > 0) {
			line_accum[line_accum_len] = '\0';
			dispatch_line(line_accum);
			flush_events();
			line_accum_len = 0;
		}
		return -1;
	}

	if (quit_requested) return -1;

	// 3. Emit a "> " prompt only when the emulator is genuinely ready for
	// the next input: RUN (more commands welcome -- e.g. brk while running)
	// or STOP (a command finished and any state-transition events flushed).
	// Hold the prompt during STEP -- the prompt should follow the upcoming
	// step-completion event, so a host doing `cmd("stp")` reads `RDY` +
	// `* BRK STEP …` + `> ` in one batch.
	if (produced_output && dbg_get_mode() != DMODE_STEP) {
		emit_prompt();
	}

	// 4. If stopped AND no buffered input AND not at EOF, block briefly on
	// stdin so we don't busy-spin. With buffered input or at EOF, the next
	// tick processes / exits without waiting.
	if (dbg_get_mode() == DMODE_STOP) {
		if (line_accum_len == 0 && !stdin_eof) {
			dbg_console_wait_readable(100);
		}
		return 1; // hold; CPU does not step
	}
	return 0; // let CPU step
}

// ---------------------------------------------------------------------------
// Frontend vtable + init/shutdown.
// ---------------------------------------------------------------------------

static const dbg_frontend_t stdio_frontend = {
	.tick      = stdio_tick,
	.on_break  = stdio_on_break,
	.on_resume = stdio_on_resume,
};

void debugger_stdio_init(void) {
	// Line-buffered stdout so command responses flush at '\n' without
	// requiring an explicit fflush after every byte.
	setvbuf(stdout, NULL, _IOLBF, 0);
	dbg_console_init();   // non-blocking stdin; raw mode if interactive
	term_init();          // editor state; adopts the console's tty verdict
	dbg_console_install_signal_handlers();
	dbg_register_frontend(&stdio_frontend);
	emit_banner();
	emit_prompt();  // initial prompt; subsequent prompts emitted by stdio_tick
}

void debugger_stdio_shutdown(void) {
	dbg_console_restore();
	term_shutdown();
}

#endif // OS dispatch
