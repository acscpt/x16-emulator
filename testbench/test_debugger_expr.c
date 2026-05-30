// test_debugger_expr.c
//
// Standalone unit test for the condition expression evaluator
// (src/debugger_expr.c). It has no emulator dependencies, so it builds and
// runs on its own. CMake produces a `test_debugger_expr` target; run it with
// `./build/test_debugger_expr`. Exits 0 if all checks pass, 1 otherwise.
//
// A fake resolver supplies fixed operand values so results are deterministic:
//   a=5 x=10 y=$20 sp=$01ff pc=$c000 p=$a5 (n=1 v=0 z=0 c=1 i=1 d=0)
//   addr=$70 val=$aa is_read=0 is_write=1   mem[i] = i & $ff

#include "debugger_expr.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

static int passed = 0;
static int failed = 0;

static const char *VALID[] = {
	"a", "x", "y", "sp", "pc", "p", "n", "v", "z", "c", "i", "d",
	"addr", "val", "is_read", "is_write", "mem", NULL,
};

static bool valid_fn(void *ctx, const char *name) {
	(void)ctx;
	for (int i = 0; VALID[i]; i++) {
		if (!strcmp(VALID[i], name)) return true;
	}
	return false;
}

static int64_t resolve_fn(void *ctx, const char *name, bool indexed, int64_t index) {
	(void)ctx;
	if (indexed) return index & 0xff;          // mem[i]
	if (!strcmp(name, "a"))        return 5;
	if (!strcmp(name, "x"))        return 10;
	if (!strcmp(name, "y"))        return 0x20;
	if (!strcmp(name, "sp"))       return 0x01ff;
	if (!strcmp(name, "pc"))       return 0xc000;
	if (!strcmp(name, "p"))        return 0xa5;
	if (!strcmp(name, "n"))        return 1;
	if (!strcmp(name, "v"))        return 0;
	if (!strcmp(name, "z"))        return 0;
	if (!strcmp(name, "c"))        return 1;
	if (!strcmp(name, "i"))        return 1;
	if (!strcmp(name, "d"))        return 0;
	if (!strcmp(name, "addr"))     return 0x70;
	if (!strcmp(name, "val"))      return 0xaa;
	if (!strcmp(name, "is_read"))  return 0;
	if (!strcmp(name, "is_write")) return 1;
	return 0;
}

static void check(const char *src, int64_t expect) {
	char err[80];
	struct dbg_expr *e = dbg_expr_compile(src, valid_fn, NULL, err, sizeof(err));
	if (!e) {
		printf("  FAIL  %-26s  compile error: %s\n", src, err);
		failed++;
		return;
	}
	int64_t got = dbg_expr_eval(e, resolve_fn, NULL);
	if (got != expect) {
		printf("  FAIL  %-26s  got %lld, want %lld\n", src, (long long)got, (long long)expect);
		failed++;
	} else {
		passed++;
	}
	dbg_expr_free(e);
}

static void check_err(const char *src) {
	char err[80];
	struct dbg_expr *e = dbg_expr_compile(src, valid_fn, NULL, err, sizeof(err));
	if (e) {
		printf("  FAIL  %-26s  expected a parse error\n", src);
		dbg_expr_free(e);
		failed++;
		return;
	}
	passed++;
}

int main(void) {
	// Literals: decimal, and hex only with a prefix.
	check("0", 0);
	check("42", 42);
	check("$10", 16);
	check("0x10", 16);
	check("$ff", 255);
	check("0xFF", 255);

	// Arithmetic, with divide/modulo-by-zero guarded to 0.
	check("2 + 3", 5);
	check("10 - 4", 6);
	check("3 * 4", 12);
	check("20 / 3", 6);
	check("20 % 3", 2);
	check("7 / 0", 0);
	check("7 % 0", 0);

	// Precedence (C rules).
	check("2 + 3 * 4", 14);
	check("(2 + 3) * 4", 20);
	check("2 * 3 + 4", 10);
	check("1 + 2 << 1", 6);     // (1+2) << 1
	check("8 >> 1 + 1", 2);     // 8 >> (1+1)
	check("1 | 2 & 3", 3);      // 1 | (2&3)
	check("(1 | 2) & 3", 3);

	// Bitwise.
	check("$f0 & $0f", 0);
	check("$f0 | $0f", 255);
	check("$ff ^ $0f", 0xf0);
	check("~0 & $ff", 255);
	check("1 << 4", 16);
	check("256 >> 4", 16);

	// Comparison and logical (0/1 results).
	check("5 == 5", 1);
	check("5 == 6", 0);
	check("5 != 6", 1);
	check("3 < 5", 1);
	check("5 <= 5", 1);
	check("5 > 3", 1);
	check("5 >= 6", 0);
	check("1 && 1", 1);
	check("1 && 0", 0);
	check("0 || 1", 1);
	check("0 || 0", 0);
	check("!0", 1);
	check("!5", 0);
	check("!(3 > 5)", 1);

	// Short-circuit (right side guarded would-be div by zero never runs).
	check("0 && (1 / 0)", 0);
	check("1 || (1 / 0)", 1);

	// Signedness.
	check("5 - 10", -5);
	check("5 - 10 < 0", 1);
	check("-3", -3);
	check("-3 + 5", 2);
	check("~5", -6);

	// Operands.
	check("a", 5);
	check("a + x", 15);
	check("a == 5", 1);
	check("x > a", 1);
	check("y", 0x20);
	check("p & $80", 0x80);
	check("n", 1);
	check("c == 1 && n == 1", 1);
	check("v || z", 0);
	check("addr == $70", 1);
	check("val >= $80", 1);
	check("is_write", 1);
	check("is_read", 0);
	check("is_write && addr == $70", 1);
	check("(p & $01) == c", 1);

	// mem[...] with literal, register, and expression subscripts.
	check("mem[$70]", 0x70);
	check("mem[a]", 5);
	check("mem[$10 + 1]", 0x11);
	check("mem[0] == 0", 1);
	check("mem[addr] == addr", 1);

	// Larger expressions.
	check("(a > 3 && a < 10) || x == 0", 1);
	check("x * 8 + 2", 82);

	// Parse errors.
	check_err("");
	check_err("1 +");
	check_err("1 2");
	check_err("ff");        // bare hex is an unknown identifier
	check_err("foo");       // unknown variable
	check_err("a = 5");     // assignment is not allowed
	check_err("(1 + 2");    // unbalanced parenthesis
	check_err("mem[1");     // unbalanced bracket
	check_err("* 5");       // leading binary operator
	check_err("a &");       // dangling operator

	printf("\n%d passed, %d failed\n", passed, failed);
	return failed ? 1 : 0;
}
