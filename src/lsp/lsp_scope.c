#include "lsp_scope.h"

#include "diag.h"
#include "expr.h"
#include "types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Destination of the collection currently in flight, or NULL. */
static LspBinding *out_       = NULL;
static int         cap_       = 0;
static int        *count_     = NULL;
static bool        truncated_ = false;
/* Copied, not aliased: the caller's temp-file path outlives the call it was
 * built in, but nothing guarantees it outlives the collection. */
static char        only_file_[1024];
static bool        filter_    = false;

/* -------------------------------------------------------------------------
 * Recording
 * --------------------------------------------------------------------- */

static void copy_field(char *dst, size_t cap, const char *src) {
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

int lsp_scope_record(const LspBinding *b) {
    if (!out_ || !count_ || !b) return 0;
    if (*count_ >= cap_) { truncated_ = true; return 0; }
    out_[(*count_)++] = *b;
    return 1;
}

/* Record one local `Binding` visible over [scope_start, scope_end).
 *
 * A global is skipped: LspSymbol already owns those, and a name in both
 * tables would make "is this a local?" depend on which table was asked
 * first. So is a synthesized name (`__fn_774`, instance methods) -- for the
 * reason lsp_collect gives, with one addition: those bindings have spans in
 * expanded source, so an edit against one would land on unrelated bytes. */
static void record_binding(const Binding *b, LspBindKind kind, int depth,
                           uint32_t scope_start, uint32_t scope_end) {
    if (!out_ || !b || !b->name || !b->name->name) return;
    if (b->is_global || b->is_synthesized) return;
    if (scope_end <= scope_start) return;
    if (filter_) {
        const char *fp = diag_file_path(b->span.file_id);
        if (!fp || strcmp(fp, only_file_) != 0) return;
    }

    /* Everything below is in the coordinates of the file on disk.
     *
     * A sweet-exp buffer reaches the elaborator as transformed s-expression
     * text, so the raw offsets index a string the user has never seen -- a
     * `let` binder that lands 13 bytes off, which is a highlight on the wrong
     * word and a rename on the wrong bytes. The scope bounds travel with the
     * binder, so they are translated too. */
    /* A scope that starts at byte zero is one we failed to compute, not one
     * that genuinely begins at the first byte of the file: a binder is always
     * inside a form, so its uses always start after something. Dropping it
     * beats recording a binding whose region is the whole file -- that is the
     * exact wrong answer, and it is the one every consumer would then trust. */
    if (scope_start == 0) return;

    Span sp    = diag_translate_span(b->span);
    Span start = diag_translate_span(
        span_from_offsets(b->span.file_id, scope_start, scope_start));
    Span stop  = diag_translate_span(
        span_from_offsets(b->span.file_id, scope_end, scope_end));
    scope_start = start.off_start;
    scope_end   = stop.off_start;
    if (scope_end <= scope_start) return;

    LspBinding rec;
    memset(&rec, 0, sizeof(rec));
    copy_field(rec.name, sizeof(rec.name), b->name->name);
    copy_field(rec.type_str, sizeof(rec.type_str), type_name(b->type));
    rec.kind          = kind;
    rec.def_line      = sp.line;
    rec.def_col_start = sp.col_start;
    rec.def_col_end   = sp.col_end;
    rec.def_off_start = sp.off_start;
    rec.def_off_end   = sp.off_end;
    rec.scope_start_off = scope_start;
    rec.scope_end_off   = scope_end;
    rec.depth         = depth;

    /* A binder with no byte span is macro-introduced: its "position" is a
     * point in expanded source that does not exist in the file. Keep the
     * record -- shadowing still has to be *seen* so a rename over the
     * shadowed name can refuse -- but leave def_line at 0, which is the
     * signal every consumer reads as "no edit here". */
    if (rec.def_off_end <= rec.def_off_start) {
        rec.def_line      = 0;
        rec.def_off_start = 0;
        rec.def_off_end   = 0;
    }
    if (getenv("TUR_LSP_SCOPE_DEBUG"))
        fprintf(stderr, "scope: %-20s kind=%d depth=%d def=[%u,%u) scope=[%u,%u)\n",
                rec.name, (int)rec.kind, rec.depth, rec.def_off_start,
                rec.def_off_end, rec.scope_start_off, rec.scope_end_off);
    lsp_scope_record(&rec);
}

/* -------------------------------------------------------------------------
 * The walk
 *
 * A partial walk on purpose. Every kind it does not model falls through to
 * its children where they are obvious and otherwise stops, and a binding it
 * never saw simply is not in the table -- which lsp_scope_lookup_at reports
 * as NULL, which every consumer reads as "global", which is exactly the
 * behaviour that shipped before this file existed. A missing case is a
 * missing improvement, never a wrong answer.
 * --------------------------------------------------------------------- */

/* Recursion guard. The elaborated tree is finite, but a bug that made it
 * cyclic would hang the editor rather than the compiler, which is the worse
 * place to find out. */
#define SCOPE_DEPTH_MAX 256

static void walk(const Expr *x, int depth, uint32_t encl_end);

/* The end of the region a form's bindings are visible in.
 *
 * The form's own span when it has one, and the enclosing region's end when it
 * does not. Falling back rather than dropping the binding matters for a
 * macro-expanded body: the binder has no span, but its *uses* do, and bounding
 * them by the enclosing function is still narrower than the whole file. */
static uint32_t region_end(const Expr *x, uint32_t encl_end) {
    if (x && x->span.off_end > x->span.off_start) return x->span.off_end;
    return encl_end;
}

static void walk_items(Expr **items, uint32_t n, int depth, uint32_t encl_end) {
    for (uint32_t i = 0; i < n; i++) walk(items[i], depth, encl_end);
}

/* A function definition: parameters, then the body one level deeper. */
static void walk_fn(const FnDef *fn, LspBindKind kind, int depth,
                    uint32_t encl_end) {
    if (!fn) return;
    uint32_t body_end = region_end(fn->body, encl_end);
    for (uint32_t i = 0; i < fn->n_params; i++) {
        const Binding *p = fn->params ? fn->params[i] : NULL;
        if (!p) continue;
        /* A parameter's uses start after its own binder and run to the end of
         * the body. The binder itself is the definition range, which
         * lsp_scope_lookup_at consults separately -- so a caret on the
         * parameter name resolves to the parameter, which is the bug c2mp's
         * S11.3 records and the reason that range exists. */
        uint32_t from = p->span.off_end > 0 ? p->span.off_end : 0;
        record_binding(p, kind, depth + 1, from, body_end);
    }
    walk(fn->body, depth + 1, body_end);
}

static void walk_pattern(const MatchPattern *pat, int depth,
                         uint32_t scope_start, uint32_t scope_end) {
    if (!pat) return;
    if (pat->is_var && pat->var_binding)
        record_binding(pat->var_binding, LSP_BIND_PATTERN, depth,
                       scope_start, scope_end);
    for (uint32_t i = 0; i < pat->n_bindings; i++) {
        if (pat->bindings && pat->bindings[i])
            record_binding(pat->bindings[i], LSP_BIND_PATTERN, depth,
                           scope_start, scope_end);
    }
}

static void walk(const Expr *x, int depth, uint32_t encl_end) {
    if (!x || depth > SCOPE_DEPTH_MAX) return;
    if (out_ && count_ && *count_ >= cap_) { truncated_ = true; return; }

    switch (x->kind) {
    case EX_PROGRAM:
        walk_items(x->as.program.items, x->as.program.n, depth, encl_end);
        return;
    case EX_DEFMODULE:
        if (x->as.defmodule_.mod)
            walk_items(x->as.defmodule_.mod->body,
                       x->as.defmodule_.mod->n_body, depth, encl_end);
        return;

    case EX_FN_DEF:
        walk_fn(x->as.fn_def_.fn, LSP_BIND_PARAM, depth,
                region_end(x, encl_end));
        return;
    case EX_FN:
        walk_fn(x->as.fn_.fn, LSP_BIND_FN_PARAM, depth,
                region_end(x, encl_end));
        return;
    case EX_CLOSURE:
        if (x->as.closure_.closure)
            walk_fn(x->as.closure_.closure->fn, LSP_BIND_FN_PARAM, depth,
                    region_end(x, encl_end));
        return;

    case EX_LET:
    case EX_LETREC: {
        uint32_t end = region_end(x, encl_end);
        bool rec = (x->kind == EX_LETREC);
        for (uint32_t i = 0; i < x->as.let_.n; i++) {
            const LetBinding *lb = &x->as.let_.bindings[i];
            walk(lb->init, depth + 1, end);
            if (!lb->binding) continue;
            /* Sequential `let`: a name is live from the end of its own init,
             * so the OLD binding is what `(let [x (+ x 1)] ...)` reads. A
             * `letrec` (and the named-let / loop it lowers from) is live from
             * the start of the group, because that is what makes the
             * recursive reference resolve to itself. */
            uint32_t from = rec ? x->span.off_start : 0;
            if (!rec) {
                from = lb->init && lb->init->span.off_end > 0
                         ? lb->init->span.off_end
                         : lb->binding->span.off_end;
            }
            record_binding(lb->binding,
                           rec ? LSP_BIND_LOOP : LSP_BIND_LET,
                           depth + 1, from, end);
        }
        walk(x->as.let_.body, depth + 1, end);
        return;
    }

    case EX_MATCH: {
        uint32_t end = region_end(x, encl_end);
        walk(x->as.match_.scrutinee, depth, end);
        for (uint32_t i = 0; i < x->as.match_.n_arms; i++) {
            const MatchArm *arm = &x->as.match_.arms[i];
            /* An arm's binders are visible in that arm's guard and body and
             * nowhere else -- which is the whole point of recording them: two
             * arms binding the same name are two different variables. */
            uint32_t arm_start = arm->body ? arm->body->span.off_start : 0;
            uint32_t arm_end   = arm->body ? region_end(arm->body, end) : end;
            if (arm->guard && arm->guard->span.off_start > 0 &&
                arm->guard->span.off_start < arm_start)
                arm_start = arm->guard->span.off_start;
            walk_pattern(&arm->pattern, depth + 1, arm_start, arm_end);
            walk(arm->guard, depth + 1, arm_end);
            walk(arm->body, depth + 1, arm_end);
        }
        return;
    }

    case EX_DEF:
        walk(x->as.def_.init, depth, encl_end);
        return;
    case EX_IF:
        walk(x->as.if_.cond, depth, encl_end);
        walk(x->as.if_.then_, depth, encl_end);
        walk(x->as.if_.else_or_null, depth, encl_end);
        return;
    case EX_DO:
        walk_items(x->as.do_.items, x->as.do_.n, depth, encl_end);
        return;
    case EX_WHILE:
        walk(x->as.while_.cond, depth, encl_end);
        walk(x->as.while_.body, depth, encl_end);
        return;
    case EX_SET:
        walk(x->as.set_.value, depth, encl_end);
        return;
    case EX_CALL:
        walk(x->as.call_.fn_expr, depth, encl_end);
        walk_items(x->as.call_.args, x->as.call_.n_args, depth, encl_end);
        return;
    case EX_BUILTIN:
        walk_items(x->as.builtin.args, x->as.builtin.n, depth, encl_end);
        return;
    case EX_MAKE_STRUCT:
        walk_items(x->as.make_struct_.field_values,
                   x->as.make_struct_.n_fields, depth, encl_end);
        return;
    case EX_GET_FIELD:
        walk(x->as.get_field_.struct_expr, depth, encl_end);
        return;
    case EX_SET_FIELD:
        walk(x->as.set_field_.receiver, depth, encl_end);
        walk(x->as.set_field_.value, depth, encl_end);
        return;
    case EX_RETURN:
        walk(x->as.return_.value, depth, encl_end);
        return;
    case EX_DEFER:
        walk(x->as.defer_.body, depth, encl_end);
        return;
    case EX_ASCRIBE:
        walk(x->as.ascribe_.inner, depth, encl_end);
        return;
    case EX_CAST:
        walk(x->as.cast_.expr, depth, encl_end);
        return;
    case EX_REINTERPRET:
        walk(x->as.reinterpret_.expr, depth, encl_end);
        return;
    case EX_REF:
        walk(x->as.ref_.expr, depth, encl_end);
        return;
    case EX_DEREF:
        walk(x->as.deref_.expr, depth, encl_end);
        return;
    case EX_HANDLE: {
        const HandleExpr *h = x->as.handle_.handle;
        if (!h) return;
        uint32_t end = region_end(x, encl_end);
        walk(h->body, depth, end);
        for (uint8_t i = 0; i < h->n_cases; i++)
            walk(h->cases[i].body, depth + 1, end);
        return;
    }

    default:
        return;
    }
}

/* -------------------------------------------------------------------------
 * Collection lifecycle + lookup
 * --------------------------------------------------------------------- */

void lsp_scope_begin(LspBinding *out, int cap, int *count_out,
                     const char *only_file) {
    out_       = out;
    cap_       = cap;
    count_     = count_out;
    truncated_ = false;
    filter_    = false;
    only_file_[0] = '\0';
    if (only_file && *only_file) {
        size_t n = strlen(only_file);
        if (n >= sizeof(only_file_)) n = sizeof(only_file_) - 1;
        memcpy(only_file_, only_file, n);
        only_file_[n] = '\0';
        filter_ = true;
    }
    if (count_) *count_ = 0;
}

bool lsp_scope_active(void) { return out_ != NULL; }

bool lsp_scope_truncated(void) { return truncated_; }

void lsp_scope_program(const struct Expr *prog_) {
    const Expr *prog = (const Expr *)prog_;
    if (!out_ || !prog || prog->kind != EX_PROGRAM) return;
    walk(prog, 0, 0xFFFFFFFFu);
}

void lsp_scope_end(void) {
    out_   = NULL;
    cap_   = 0;
    count_ = NULL;
}

const LspBinding *lsp_scope_lookup_at(const LspBinding *tab, int count,
                                      size_t off, const char *name) {
    if (!tab || count <= 0 || !name || !*name) return NULL;
    const LspBinding *best = NULL;
    for (int i = 0; i < count; i++) {
        const LspBinding *b = &tab[i];
        if (strcmp(b->name, name) != 0) continue;
        bool on_binder = b->def_off_end > b->def_off_start &&
                         off >= b->def_off_start && off < b->def_off_end;
        bool in_scope  = off >= b->scope_start_off && off < b->scope_end_off;
        if (!on_binder && !in_scope) continue;
        /* Innermost wins, and the answer is one binding. Between two at the
         * same depth -- sibling `let`s whose regions a fallback widened to the
         * same enclosing body -- the later binder is the one in effect. */
        if (!best || b->depth > best->depth ||
            (b->depth == best->depth &&
             b->scope_start_off > best->scope_start_off))
            best = b;
    }
    return best;
}
