/* refine_smtlib.c -- RT2: normalized VC -> SMT-LIB2 serializer.
 *
 * Term encoding is the obvious structural map.  Because nonlinear and measure
 * terms were already abstracted to uninterpreted functions when the VC was
 * built (refine_collect.c), this file has no special cases for them -- they
 * are just `declare-fun` applications. */

#include "refine_smtlib.h"

#include <math.h>
#include <string.h>

static const char *sort_name(VCSort s) {
    switch (s) {
        case VS_REAL: return "Real";
        case VS_BOOL: return "Bool";
        default:      return "Int";
    }
}

static void emit_term(const RefineVC *vc, const VCTerm *t, Buf *out);

static void emit_nary(const RefineVC *vc, const VCTerm *t, const char *op, Buf *out) {
    buf_printf(out, "(%s", op);
    for (uint32_t i = 0; i < t->n; i++) { buf_putc(out, ' '); emit_term(vc, t->kids[i], out); }
    buf_putc(out, ')');
}

static void emit_term(const RefineVC *vc, const VCTerm *t, Buf *out) {
    if (!t) { buf_puts(out, "true"); return; }
    switch (t->op) {
        case VC_TRUE:  buf_puts(out, "true");  return;
        case VC_FALSE: buf_puts(out, "false"); return;
        case VC_CONST_INT:
            if (t->as.i < 0) buf_printf(out, "(- %lld)", (long long)(-t->as.i));
            else             buf_printf(out, "%lld", (long long)t->as.i);
            return;
        case VC_CONST_REAL: {
            double v = t->as.r;
            if (v < 0) buf_printf(out, "(- %.17g)", -v);
            else       buf_printf(out, "%.17g", v);
            return;
        }
        case VC_VAR:
            buf_puts(out, t->as.idx < vc->n_vars ? vc->vars[t->as.idx].name : "|?var|");
            return;
        case VC_APP: {
            const VCUFunc *u = t->as.idx < vc->n_ufuncs ? &vc->ufuncs[t->as.idx] : NULL;
            buf_printf(out, "(%s", u ? u->name : "|?fn|");
            for (uint32_t i = 0; i < t->n; i++) { buf_putc(out, ' '); emit_term(vc, t->kids[i], out); }
            buf_putc(out, ')');
            return;
        }
        case VC_ADD: emit_nary(vc, t, "+", out); return;
        case VC_SUB: emit_nary(vc, t, "-", out); return;
        case VC_MUL: emit_nary(vc, t, "*", out); return;
        case VC_DIV: emit_nary(vc, t, vc->has_real ? "/" : "div", out); return;
        case VC_MOD: emit_nary(vc, t, "mod", out); return;
        case VC_NEG: emit_nary(vc, t, "-", out); return;
        case VC_EQ:  emit_nary(vc, t, "=", out); return;
        case VC_LT:  emit_nary(vc, t, "<", out); return;
        case VC_LE:  emit_nary(vc, t, "<=", out); return;
        case VC_AND: emit_nary(vc, t, "and", out); return;
        case VC_OR:  emit_nary(vc, t, "or", out); return;
        case VC_NOT: emit_nary(vc, t, "not", out); return;
        case VC_IMPLIES: emit_nary(vc, t, "=>", out); return;
    }
    buf_puts(out, "true");
}

void refine_smtlib_emit(const RefineVC *vc, Buf *out) {
    if (!vc) return;
    buf_printf(out, "(set-logic %s)\n", vc->has_real ? "QF_UFLRA" : "QF_UFLIA");

    for (uint32_t i = 0; i < vc->n_vars; i++)
        buf_printf(out, "(declare-const %s %s)\n",
                   vc->vars[i].name, sort_name(vc->vars[i].sort));

    for (uint32_t i = 0; i < vc->n_ufuncs; i++) {
        const VCUFunc *u = &vc->ufuncs[i];
        buf_printf(out, "(declare-fun %s (", u->name);
        for (uint32_t j = 0; j < u->arity; j++)
            buf_printf(out, "%s%s", j ? " " : "", vc->has_real ? "Real" : "Int");
        buf_printf(out, ") %s)\n", sort_name(u->sort));
    }

    for (uint32_t i = 0; i < vc->n_hyps; i++) {
        buf_puts(out, "(assert ");
        emit_term(vc, vc->hyps[i], out);
        buf_puts(out, ")\n");
    }

    buf_puts(out, "(assert (not ");
    emit_term(vc, vc->goal, out);
    buf_puts(out, "))\n(check-sat)\n(get-model)\n");
}
