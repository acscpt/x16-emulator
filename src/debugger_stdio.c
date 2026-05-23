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

#elif defined(_WIN32)
// TODO: non-blocking stdin on Windows (WaitForSingleObject on
// STD_INPUT_HANDLE, or a worker-thread reader). Not yet implemented;
// the protocol design has not been validated against MinGW stdin quirks.
#include <stdio.h>
void debugger_stdio_init(void) {
	fprintf(stderr, "-debugstdio is not yet supported on Windows.\n");
}
void debugger_stdio_shutdown(void) {}

#else
// ===========================================================================
// POSIX implementation.
// ===========================================================================

#include "debugger_core.h"
#include "memory.h"
#include "cpu/fake6502.h"
#include "cpu/registers.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
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

// ---------------------------------------------------------------------------
// Detach flag (set by `qit`).
// ---------------------------------------------------------------------------

static bool quit_requested = false;

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

static void cmd_sbp(int argc, char **argv) {
	uint32_t bank, addr;
	if (argc != 2 || !parse_hex(argv[0], &bank, 0xff) || !parse_hex(argv[1], &addr, 0xffff)) {
		err_msg("usage: sbp <bank> <addr>");
		return;
	}
	// Note: single-breakpoint semantics in this phase (core supports one BP).
	// Setting a new sbp overwrites any existing one. Multi-BP support arrives
	// when the core API grows.
	struct breakpoint bp = { .pc = (int)addr, .bank = (uint8_t)bank, .x16Bank = -1 };
	dbg_set_breakpoint(bp);
	rdy();
}

static void cmd_cbp(int argc, char **argv) {
	uint32_t bank, addr;
	if (argc != 2 || !parse_hex(argv[0], &bank, 0xff) || !parse_hex(argv[1], &addr, 0xffff)) {
		err_msg("usage: cbp <bank> <addr>");
		return;
	}
	struct breakpoint bp = dbg_get_breakpoint();
	if (bp.pc == (int)addr && bp.bank == (uint8_t)bank) {
		struct breakpoint clear = { .pc = -1, .bank = 0, .x16Bank = -1 };
		dbg_set_breakpoint(clear);
		rdy();
	} else {
		err_msg("no such breakpoint");
	}
}

static void cmd_lbp(int argc, char **argv) {
	(void)argv;
	if (argc != 0) { err_msg("lbp takes no args"); return; }
	struct breakpoint bp = dbg_get_breakpoint();
	if (bp.pc >= 0) {
		printf("%02x %04x\n", bp.bank, (uint16_t)bp.pc);
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
	while (count > 0) {
		int line = count > 16 ? 16 : (int)count;
		printf("%05x:", cur);
		for (int i = 0; i < line; i++) {
			printf(" %02x", dbg_read_vram((cur + i) & 0x1ffff));
		}
		printf("\n");
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
	while (count > 0) {
		int line = count > 16 ? 16 : (int)count;
		printf("%04x:", cur);
		for (int i = 0; i < line; i++) {
			printf(" %02x", dbg_read_mem((uint8_t)bank, (uint16_t)(cur + i), -1));
		}
		printf("\n");
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
// Command dispatch table.
// ---------------------------------------------------------------------------

typedef void (*cmd_fn)(int argc, char **argv);

static const struct {
	const char *name;
	cmd_fn      fn;
} commands[] = {
	// execution control
	{"brk", cmd_brk},
	{"cnt", cmd_cnt},
	{"stp", cmd_stp},
	{"sov", cmd_sov},
	{"rst", cmd_rst},
	// breakpoints
	{"sbp", cmd_sbp},
	{"cbp", cmd_cbp},
	{"lbp", cmd_lbp},
	// registers
	{"reg", cmd_reg},
	{"srg", cmd_srg},
	// memory (RAM)
	{"mem", cmd_mem},
	{"wmm", cmd_wmm},
	{"fil", cmd_fil},
	// memory (VRAM)
	{"vmr", cmd_vmr},
	{"vmw", cmd_vmw},
	// disassembly
	{"dis", cmd_dis},
	// snapshot panels (parity with SDL)
	{"stk", cmd_stk},
	{"zpr", cmd_zpr},
	{"vrg", cmd_vrg},
	{"clk", cmd_clk},
	// session
	{"mod", cmd_mod},
	{"ver", cmd_ver},
	{"qit", cmd_qit},
	{NULL,  NULL},
};

static void dispatch_line(char *line) {
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
// Reads raw bytes from STDIN_FILENO into a private accumulator and extracts
// complete lines from it. Using fgets here would be a trap: it pulls bytes
// from the pipe into libc's stdin buffer in chunks, and poll() can't see
// libc-buffered data -- so after one fgets, poll() falsely reports "no data"
// while a queued line is still sitting in libc. Going through raw read()
// keeps everything visible to poll().
// ---------------------------------------------------------------------------

#define LINE_ACCUM_SZ 8192

static char   line_accum[LINE_ACCUM_SZ];
static size_t line_accum_len = 0;
static bool   stdin_eof      = false;

static void make_stdin_nonblocking(void) {
	int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
	if (flags >= 0) {
		(void)fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
	}
}

// Pull whatever is available right now into line_accum. Returns -1 on EOF.
static int drain_stdin(void) {
	while (line_accum_len < LINE_ACCUM_SZ - 1) {
		ssize_t n = read(STDIN_FILENO,
		                 line_accum + line_accum_len,
		                 LINE_ACCUM_SZ - 1 - line_accum_len);
		if (n == 0) {
			stdin_eof = true;
			return -1;
		}
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
			stdin_eof = true;
			return -1;
		}
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
		flush_events();
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

	// 3. If stopped AND no buffered input AND not at EOF, block briefly on
	// stdin so we don't busy-spin. With buffered input or at EOF, the next
	// tick processes / exits without waiting.
	if (dbg_get_mode() == DMODE_STOP) {
		if (line_accum_len == 0 && !stdin_eof) {
			struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
			(void)poll(&pfd, 1, 100);
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
	make_stdin_nonblocking();
	dbg_register_frontend(&stdio_frontend);
}

void debugger_stdio_shutdown(void) {
	// no-op for now
}

#endif // OS dispatch
