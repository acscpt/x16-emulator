// debugger_expr.h
//
// A small read-only expression evaluator for debugger conditions (the
// `if <expr>` clause on breakpoints and watchpoints).
//
// It is a predicate, not a language: it reads current machine state and
// returns a value, with no assignment, variables of its own, or side
// effects. The result is read as a boolean (non-zero is true).
//
// The evaluator is machine-neutral. It understands C-style operators and
// integer literals; the operand vocabulary (register names, mem[...], etc.)
// is supplied by the host through two callbacks: a validity check used at
// compile time (so unknown names are a parse error with a clear message),
// and a resolver used at eval time. Working values are signed 64-bit so
// intermediate arithmetic and comparisons do not wrap; operands are read at
// their natural width by the host and widened in.

#ifndef DEBUGGER_EXPR_H
#define DEBUGGER_EXPR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

struct dbg_expr;

// True if `name` is a valid operand in the host's vocabulary.
typedef bool (*dbg_expr_valid_fn)(void *ctx, const char *name);

// Return the current value of operand `name`. For an indexed operand such
// as `mem[...]`, `indexed` is true and `index` is the evaluated subscript;
// otherwise `indexed` is false and `index` is 0.
typedef int64_t (*dbg_expr_resolve_fn)(void *ctx, const char *name, bool indexed, int64_t index);

// Compile `src`. On success returns a non-NULL handle. On a parse error
// returns NULL and, if errbuf is non-NULL, writes a short message into it.
// `valid` / `valid_ctx` validate operand names while parsing.
struct dbg_expr *dbg_expr_compile(const char *src,
                                  dbg_expr_valid_fn valid, void *valid_ctx,
                                  char *errbuf, size_t errlen);

// Evaluate, resolving operands via `resolve` / `ctx`. Returns the signed
// 64-bit result; callers treat non-zero as true.
int64_t dbg_expr_eval(const struct dbg_expr *e, dbg_expr_resolve_fn resolve, void *ctx);

void dbg_expr_free(struct dbg_expr *e);

#endif
