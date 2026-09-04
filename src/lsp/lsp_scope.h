#ifndef TUR_LSP_SCOPE_H
#define TUR_LSP_SCOPE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct Expr;

/* -------------------------------------------------------------------------
 * Lexical scope resolution (editor-intelligence-follow-through-plan, S1)
 *
 * lsp_collect.c records every *global* binding. That is all a hover or an
 * outline ever needed, and it is not enough for any question about a name the
 * user is standing inside: a parameter named `x` has no entry at all, so
 * highlight scans the whole buffer for it, completion never offers it, and
 * rename cannot be written -- renaming `total` when a `let` shadows a global
 * `total` silently changes what the program means.
 *
 * This is the missing half: one record per *local* binding, carrying the
 * source region the binding is visible in. Collected the same way symbols are
 * -- bracketed by begin/end, fed from the one elaboration hook lsp_collect
 * already sits on, so there is no second traversal and no second front end.
 *
 * Single-threaded: one collection is in flight at a time.
 * --------------------------------------------------------------------- */

/* What introduced a binding.
 *
 * Deliberately not the LSP wire numbering, for the reason LspSymKind gives:
 * CompletionItemKind and SymbolKind number the same distinctions differently,
 * and one neutral tag maps to both. */
typedef enum {
    LSP_BIND_UNKNOWN = 0,
    LSP_BIND_PARAM,     /* defn parameter */
    LSP_BIND_FN_PARAM,  /* fn / closure (lambda) parameter */
    LSP_BIND_LET,       /* let binding */
    LSP_BIND_LOOP,      /* letrec / named-let / loop binder */
    LSP_BIND_PATTERN,   /* match-arm destructuring binder */
} LspBindKind;

/* One lexical binding: a name, where it was bound, and the region it is
 * visible in.
 *
 * Two offset ranges, not one, and the split is what makes rename safe. The
 * *definition* range is the binder itself; the *scope* range is where uses of
 * it live. They are disjoint, and the gap between them is a binding's own
 * initializer -- `(let [x (+ x 1)] x)` binds a new `x` whose init reads the
 * OLD one. c2mp's S11.3 started a binding's scope at the body and found the
 * caret on the binder resolving to whatever global shared its name; starting
 * it at the binder instead fixes that and breaks the init, which for a
 * highlight is cosmetic and for a rename is corruption. Carrying both ranges
 * costs one comparison and gets both right.
 *
 * `def_line == 0` means the binder has no span in the file the user is
 * editing -- a macro-introduced binding. Every consumer reads that as "not
 * renameable, not highlightable" rather than as an offset. */
typedef struct {
    char        name[128];
    char        type_str[128];  /* rendered type, or "" */
    LspBindKind kind;
    uint32_t    def_line;       /* 1-based; 0 == no source span (macro) */
    uint32_t    def_col_start;  /* 1-based, inclusive */
    uint32_t    def_col_end;    /* 1-based, exclusive */
    uint32_t    def_off_start;  /* byte offset of the binder, inclusive */
    uint32_t    def_off_end;    /* byte offset of the binder, exclusive */
    uint32_t    scope_start_off;/* first byte a use may appear at */
    uint32_t    scope_end_off;  /* one past the last */
    int         depth;          /* nesting depth; innermost wins */
} LspBinding;

/* Start collecting into out[0..cap-1]. *count_out is zeroed and then tracks
 * how many bindings were written.
 *
 * `only_file` is the path whose locals are wanted, and it is not optional in
 * practice: a compile of one buffer elaborates the whole prepended stdlib
 * with it, and the stdlib's own `let`s and parameters are thousands of local
 * bindings belonging to files the user is not editing. Collecting them would
 * overflow any cap worth allocating and mark the table truncated -- which,
 * per the rule above, refuses every rename in the session. Filtering at
 * record time keeps the table to one file's worth. NULL collects everything,
 * which only a test wants. */
void lsp_scope_begin(LspBinding *out, int cap, int *count_out,
                     const char *only_file);

/* True between begin() and end(). The compiler's collection hook consults
 * this so the walk is only paid for when someone asked for scopes. */
bool lsp_scope_active(void);

/* True when the current (or just-finished) collection hit the cap.
 *
 * Reported rather than swallowed: a rename that saw a truncated binding table
 * cannot tell "not a local" from "a local we ran out of room for", and the
 * difference between those two answers is an edit applied to the wrong name.
 * prepareRename refuses on this. */
bool lsp_scope_truncated(void);

/* Record every local binding reachable from `prog` (an EX_PROGRAM).
 * No-op when no collection is active or `prog` is NULL. */
void lsp_scope_program(const struct Expr *prog);

/* Append one binding to the collection in flight. Exposed for the walker and
 * for tests that stand in for the compiler. Returns 1 if it was stored. */
int lsp_scope_record(const LspBinding *b);

/* Stop collecting. The caller's array and counter keep what was written. */
void lsp_scope_end(void);

/* The innermost binding named `name` that is live at byte offset `off`, or
 * NULL -- which means "this name is not a local here", i.e. it is the global.
 *
 * A caret sitting on a binder resolves to that binder (the definition range),
 * which is what lets a rename started from the declaration work at all.
 * Pure over the caller's table, so a document answers from its own. */
const LspBinding *lsp_scope_lookup_at(const LspBinding *tab, int count,
                                      size_t off, const char *name);

#endif
