// debugger_expr.c
//
// Recursive-descent / precedence-climbing evaluator for debugger condition
// expressions. See debugger_expr.h. Parse once into a small AST, evaluate
// many times. Nodes are individually allocated and tracked in a flat list
// for bulk free, so a parse error never leaks a partial tree.

#include "debugger_expr.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

// ---- Operators ----

enum {
	OP_LOR, OP_LAND, OP_BOR, OP_BXOR, OP_BAND,
	OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE,
	OP_SHL, OP_SHR, OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
	OP_NOT, OP_BNOT, OP_NEG, // unary
};

// Binary-operator precedence (higher binds tighter); -1 = not binary.
static int binop_prec(int op) {
	switch (op) {
		case OP_LOR:                                  return 1;
		case OP_LAND:                                 return 2;
		case OP_BOR:                                  return 3;
		case OP_BXOR:                                 return 4;
		case OP_BAND:                                 return 5;
		case OP_EQ: case OP_NE:                       return 6;
		case OP_LT: case OP_LE: case OP_GT: case OP_GE: return 7;
		case OP_SHL: case OP_SHR:                     return 8;
		case OP_ADD: case OP_SUB:                     return 9;
		case OP_MUL: case OP_DIV: case OP_MOD:        return 10;
		default:                                      return -1;
	}
}

// ---- AST ----

enum { N_NUM, N_VAR, N_MEM, N_UNOP, N_BINOP };

struct node {
	int          kind;
	int          op;       // N_UNOP / N_BINOP
	int64_t      num;      // N_NUM
	char         name[24]; // N_VAR / N_MEM
	struct node *a, *b;    // children
};

struct dbg_expr {
	struct node  *root;
	struct node **all;     // every allocated node, for bulk free
	int           count;
	int           cap;
};

static struct node *new_node(struct dbg_expr *e, int kind) {
	if (e->count == e->cap) {
		int ncap = e->cap ? e->cap * 2 : 16;
		struct node **n = realloc(e->all, (size_t)ncap * sizeof(*n));
		if (!n) return NULL;
		e->all = n;
		e->cap = ncap;
	}
	struct node *nd = calloc(1, sizeof(*nd));
	if (!nd) return NULL;
	nd->kind = kind;
	e->all[e->count++] = nd;
	return nd;
}

// ---- Tokenizer ----

enum { T_END, T_NUM, T_IDENT, T_OP, T_LPAREN, T_RPAREN, T_LBRACK, T_RBRACK, T_ERR };

struct parser {
	const char        *p;
	struct dbg_expr   *e;
	dbg_expr_valid_fn  valid;
	void              *valid_ctx;
	char              *err;
	size_t             errlen;
	bool               failed;
	// lookahead token
	int     tok;
	int64_t tok_num;
	char    tok_name[24];
	int     tok_op;
};

static void fail(struct parser *ps, const char *msg) {
	if (!ps->failed) {
		ps->failed = true;
		if (ps->err && ps->errlen) {
			snprintf(ps->err, ps->errlen, "%s", msg);
		}
	}
}

static void advance(struct parser *ps) {
	const char *p = ps->p;
	while (isspace((unsigned char)*p)) p++;
	if (*p == '\0') { ps->tok = T_END; ps->p = p; return; }

	// Number: $hex, 0x hex, or decimal. Bare hex is not allowed; hex needs
	// a $ or 0x prefix so a condition's literals are unambiguous.
	if (*p == '$' || isdigit((unsigned char)*p)) {
		int base = 10;
		if (*p == '$') { base = 16; p++; }
		else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
		char *end;
		long long v = strtoll(p, &end, base);
		if (end == p) { fail(ps, "bad number"); ps->tok = T_ERR; return; }
		ps->tok = T_NUM; ps->tok_num = (int64_t)v; ps->p = end;
		return;
	}

	// Identifier.
	if (isalpha((unsigned char)*p) || *p == '_') {
		int i = 0;
		while ((isalnum((unsigned char)*p) || *p == '_') && i < (int)sizeof(ps->tok_name) - 1) {
			ps->tok_name[i++] = *p++;
		}
		ps->tok_name[i] = '\0';
		ps->tok = T_IDENT; ps->p = p;
		return;
	}

	// Operators and brackets.
	char c = *p++;
	char n = *p;
	switch (c) {
		case '(': ps->tok = T_LPAREN; ps->p = p; return;
		case ')': ps->tok = T_RPAREN; ps->p = p; return;
		case '[': ps->tok = T_LBRACK; ps->p = p; return;
		case ']': ps->tok = T_RBRACK; ps->p = p; return;
		case '+': ps->tok = T_OP; ps->tok_op = OP_ADD; ps->p = p; return;
		case '-': ps->tok = T_OP; ps->tok_op = OP_SUB; ps->p = p; return;
		case '*': ps->tok = T_OP; ps->tok_op = OP_MUL; ps->p = p; return;
		case '/': ps->tok = T_OP; ps->tok_op = OP_DIV; ps->p = p; return;
		case '%': ps->tok = T_OP; ps->tok_op = OP_MOD; ps->p = p; return;
		case '^': ps->tok = T_OP; ps->tok_op = OP_BXOR; ps->p = p; return;
		case '~': ps->tok = T_OP; ps->tok_op = OP_BNOT; ps->p = p; return;
		case '&': if (n == '&') { ps->p = p + 1; ps->tok_op = OP_LAND; } else { ps->p = p; ps->tok_op = OP_BAND; } ps->tok = T_OP; return;
		case '|': if (n == '|') { ps->p = p + 1; ps->tok_op = OP_LOR; }  else { ps->p = p; ps->tok_op = OP_BOR; }  ps->tok = T_OP; return;
		case '=': if (n == '=') { ps->p = p + 1; ps->tok = T_OP; ps->tok_op = OP_EQ; return; } fail(ps, "use == for comparison"); ps->tok = T_ERR; return;
		case '!': if (n == '=') { ps->p = p + 1; ps->tok_op = OP_NE; } else { ps->p = p; ps->tok_op = OP_NOT; } ps->tok = T_OP; return;
		case '<': if (n == '<') { ps->p = p + 1; ps->tok_op = OP_SHL; } else if (n == '=') { ps->p = p + 1; ps->tok_op = OP_LE; } else { ps->p = p; ps->tok_op = OP_LT; } ps->tok = T_OP; return;
		case '>': if (n == '>') { ps->p = p + 1; ps->tok_op = OP_SHR; } else if (n == '=') { ps->p = p + 1; ps->tok_op = OP_GE; } else { ps->p = p; ps->tok_op = OP_GT; } ps->tok = T_OP; return;
		default: fail(ps, "unexpected character"); ps->tok = T_ERR; return;
	}
}

// ---- Parser ----

static struct node *parse_expr(struct parser *ps, int min_prec);

static struct node *parse_primary(struct parser *ps) {
	if (ps->failed) return NULL;
	if (ps->tok == T_NUM) {
		struct node *nd = new_node(ps->e, N_NUM);
		if (!nd) { fail(ps, "out of memory"); return NULL; }
		nd->num = ps->tok_num;
		advance(ps);
		return nd;
	}
	if (ps->tok == T_IDENT) {
		char name[24];
		memcpy(name, ps->tok_name, sizeof(name));
		if (ps->valid && !ps->valid(ps->valid_ctx, name)) {
			char msg[64];
			snprintf(msg, sizeof(msg), "unknown variable: %s", name);
			fail(ps, msg);
			return NULL;
		}
		advance(ps);
		if (ps->tok == T_LBRACK) {
			advance(ps);
			struct node *idx = parse_expr(ps, 0);
			if (ps->failed) return NULL;
			if (ps->tok != T_RBRACK) { fail(ps, "expected ]"); return NULL; }
			advance(ps);
			struct node *nd = new_node(ps->e, N_MEM);
			if (!nd) { fail(ps, "out of memory"); return NULL; }
			memcpy(nd->name, name, sizeof(nd->name));
			nd->a = idx;
			return nd;
		}
		struct node *nd = new_node(ps->e, N_VAR);
		if (!nd) { fail(ps, "out of memory"); return NULL; }
		memcpy(nd->name, name, sizeof(nd->name));
		return nd;
	}
	if (ps->tok == T_LPAREN) {
		advance(ps);
		struct node *inner = parse_expr(ps, 0);
		if (ps->failed) return NULL;
		if (ps->tok != T_RPAREN) { fail(ps, "expected )"); return NULL; }
		advance(ps);
		return inner;
	}
	fail(ps, "expected a value");
	return NULL;
}

static struct node *parse_unary(struct parser *ps) {
	if (ps->failed) return NULL;
	if (ps->tok == T_OP && (ps->tok_op == OP_NOT || ps->tok_op == OP_BNOT || ps->tok_op == OP_SUB)) {
		int op = (ps->tok_op == OP_SUB) ? OP_NEG : ps->tok_op;
		advance(ps);
		struct node *operand = parse_unary(ps);
		if (ps->failed) return NULL;
		struct node *nd = new_node(ps->e, N_UNOP);
		if (!nd) { fail(ps, "out of memory"); return NULL; }
		nd->op = op;
		nd->a  = operand;
		return nd;
	}
	return parse_primary(ps);
}

static struct node *parse_expr(struct parser *ps, int min_prec) {
	struct node *left = parse_unary(ps);
	if (ps->failed) return NULL;
	while (ps->tok == T_OP) {
		int prec = binop_prec(ps->tok_op);
		if (prec < 0 || prec < min_prec) break;
		int op = ps->tok_op;
		advance(ps);
		struct node *right = parse_expr(ps, prec + 1);
		if (ps->failed) return NULL;
		struct node *nd = new_node(ps->e, N_BINOP);
		if (!nd) { fail(ps, "out of memory"); return NULL; }
		nd->op = op;
		nd->a  = left;
		nd->b  = right;
		left = nd;
	}
	return left;
}

// ---- Eval ----

static int64_t eval(const struct node *nd, dbg_expr_resolve_fn resolve, void *ctx) {
	switch (nd->kind) {
		case N_NUM: return nd->num;
		case N_VAR: return resolve(ctx, nd->name, false, 0);
		case N_MEM: return resolve(ctx, nd->name, true, eval(nd->a, resolve, ctx));
		case N_UNOP: {
			int64_t v = eval(nd->a, resolve, ctx);
			switch (nd->op) {
				case OP_NOT:  return !v;
				case OP_BNOT: return ~v;
				case OP_NEG:  return -v;
			}
			return 0;
		}
		case N_BINOP: {
			// Short-circuit the logical operators.
			if (nd->op == OP_LAND) return eval(nd->a, resolve, ctx) ? (eval(nd->b, resolve, ctx) != 0) : 0;
			if (nd->op == OP_LOR)  return eval(nd->a, resolve, ctx) ? 1 : (eval(nd->b, resolve, ctx) != 0);
			int64_t l = eval(nd->a, resolve, ctx);
			int64_t r = eval(nd->b, resolve, ctx);
			switch (nd->op) {
				case OP_ADD:  return l + r;
				case OP_SUB:  return l - r;
				case OP_MUL:  return l * r;
				case OP_DIV:  return r ? l / r : 0;
				case OP_MOD:  return r ? l % r : 0;
				case OP_BAND: return l & r;
				case OP_BOR:  return l | r;
				case OP_BXOR: return l ^ r;
				case OP_SHL:  return l << (r & 63);
				case OP_SHR:  return l >> (r & 63);
				case OP_EQ:   return l == r;
				case OP_NE:   return l != r;
				case OP_LT:   return l <  r;
				case OP_LE:   return l <= r;
				case OP_GT:   return l >  r;
				case OP_GE:   return l >= r;
			}
			return 0;
		}
	}
	return 0;
}

// ---- Public API ----

struct dbg_expr *dbg_expr_compile(const char *src,
                                  dbg_expr_valid_fn valid, void *valid_ctx,
                                  char *errbuf, size_t errlen) {
	struct dbg_expr *e = calloc(1, sizeof(*e));
	if (!e) {
		if (errbuf && errlen) snprintf(errbuf, errlen, "out of memory");
		return NULL;
	}
	struct parser ps = {0};
	ps.p = src;
	ps.e = e;
	ps.valid = valid;
	ps.valid_ctx = valid_ctx;
	ps.err = errbuf;
	ps.errlen = errlen;

	advance(&ps);
	if (ps.tok == T_END) { fail(&ps, "empty condition"); }
	struct node *root = ps.failed ? NULL : parse_expr(&ps, 0);
	if (!ps.failed && ps.tok != T_END) { fail(&ps, "trailing tokens"); }

	if (ps.failed) {
		dbg_expr_free(e);
		return NULL;
	}
	e->root = root;
	return e;
}

int64_t dbg_expr_eval(const struct dbg_expr *e, dbg_expr_resolve_fn resolve, void *ctx) {
	if (!e || !e->root) return 0;
	return eval(e->root, resolve, ctx);
}

void dbg_expr_free(struct dbg_expr *e) {
	if (!e) return;
	for (int i = 0; i < e->count; i++) free(e->all[i]);
	free(e->all);
	free(e);
}
