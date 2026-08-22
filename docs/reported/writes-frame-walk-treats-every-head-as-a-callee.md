# WF2 treats every non-global head as an opaque callee, so most real bodies are UNVERIFIED

**Severity: medium.** Sound (it declines, never miscompiles), but it declines so
much that a `#writes` frame is unusable as a *fact* for almost any realistic
body -- which silently disables every consumer that requires `writes_checked`,
notably WF3's borrow widening. Filed 2026-08-22 (originally as
`writes-frame-declined-by-field-read.md`); **re-diagnosed 2026-08-22, see
"Correction" below -- the first diagnosis was too narrow and its proposed fix
was unsound.**

## Summary

`wf_walk` walks the **raw `Form`** tree. Any `(head arg ...)` whose head symbol
is not a resolvable global binding, and which receives a parameter-rooted
argument, is treated as a call to an unknowable callee and answers
`WF_UNVERIFIED` for the whole function. That catches, among others:

- a field read of a frame-named parameter -- `(.n a)`
- **every pure special form** -- `(if (= p 0) 1 2)`, `(do p 5)`
- **every arithmetic or comparison builtin** -- `(+ p 1)`, `(= p 0)`

Because UNVERIFIED is silent by design, none of this is visible without
`--dump-write-frames`.

## Minimal repro

```turmeric
(defstruct Ctr [n : int])

(defn base       [^mut a : Ctr] #writes [a] : int (set! (.n a) 1) 0)
(defn field-read [^mut a : Ctr] #writes [a] : int (set! (.n a) 1) (.n a))
(defn uses-if    [p : int] #writes [] : int (if (= p 0) 1 2))
(defn uses-do    [p : int] #writes [] : int (do p 5))
(defn uses-plus  [p : int] #writes [] : int (+ p 1))
(defn uses-let   [p : int] #writes [] : int (let [q p] q))

(defn main [] : int (println 0) 0)
```

```
$ tur run --dump-write-frames repro.tur
write-frame base:       VERIFIED   ...
write-frame field-read: UNVERIFIED ...   <-- a READ
write-frame uses-if:    UNVERIFIED ...   <-- pure control flow
write-frame uses-do:    UNVERIFIED ...   <-- pure control flow
write-frame uses-plus:  UNVERIFIED ...   <-- arithmetic
write-frame uses-let:   VERIFIED   ...
```

`uses-let` passes only by accident: `let`'s slot 1 is the binding *vector*, so
`wf_place_root_param` finds no parameter root and the callee analysis is never
entered.

The practical reading is that **WF2 verifies a frame only for a body that never
mentions a parameter inside any form other than `set!` or a call to a resolvable
global function.** In-tree that looks fine -- 7 fixtures declare `#writes`, 21
frames, 18 VERIFIED -- but those fixtures were authored to the checker's shape.
`tests/fixtures/wf1-writes-frame-honored`'s `peek` is silently UNVERIFIED today,
while its header says the fixture "compiles clean (WF2 verifies the frame)"; no
`--dump-write-frames` assertion was there to catch it.

## Root cause

`wf_walk`, `src/compiler/elab_fns.c:1560-1579`. `passes_param` is computed with
`wf_place_root_param` -- a **place (lvalue) analyzer**, written for `set!`
targets, which recurses into slot 1 of any list form. Applied to *arguments* it
reports "hands a parameter onward" for `(+ p 1)` and `(if (= p 0) ...)` alike.
The head is then resolved as a callee:

```c
Binding *callee = scope_lookup(&e->global, head);
if (!callee) {
    /* Not resolvable here (a local fn value, a typeclass method, a
     * builtin): no frame to consult, so no vouching. */
    v = wf_worse(v, WF_UNVERIFIED);
}
```

`if`, `do`, `+`, and `.n` are none of them global bindings, so all four take the
"opaque callee" branch -- a branch written for genuinely opaque *calls*, reached
here by control flow, arithmetic, and reads that cannot write anything.

## Correction: the originally proposed fix was unsound

The first version of this report proposed recognizing a field access and letting
it fall through to the recursive descent. **Do not do that.** A dotted head is
not always a field read: `elab_call.c:2626` dispatches any `name[0] == '.'` to
`elab_method_call`, so `(.foo x)` may be a method call, and a method taking a
`^mut` receiver writes through it. Admitting all dotted heads would widen
VERIFIED over real write channels -- turning a conservative checker into a wrong
one.

Telling the two apart needs the receiver's type, which the `Form` tree does not
have. That is the actual lesson: **this walk is trying to answer a semantic
question on a syntactic tree.**

## Fix directions

**Preferred: port the walk to the elaborated `Expr` tree.** The `#reads` side of
the same file already does exactly this, and the plumbing it needs is already
present:

- `rf_scan(const Expr *x, Binding **params, uint32_t n_params, ...)` at
  `elab_fns.c:2184`, called as `rf_scan(fd->body, s->params, s->n_params, ...)`
  at `:2385`, from a fixed-point pass (`:2373`) whose shape and rationale mirror
  `wf_resolve_write_frames`.
- WF2 runs at `elab_toplevel.c:2195`, i.e. after the whole unit is elaborated,
  so `fn->source_fn_def->body` is populated by then.

On `Expr`, every case that is guesswork on `Form` is decided: `EX_GET_FIELD` is
a read, `EX_BUILTIN` carries a spec whose shape `rt_builtin_shape_pure` /
`rt_builtin_shape_impure` already classify, `EX_CALL` carries `fn_binding` (and
flags an indirect or poly call explicitly), and control flow is its own node
kind rather than a symbol that failed a lookup.

**Fallback, if the port is too large to take on now:** a sound partial widening
that leaves the reported field-read case unfixed. In the call block, before the
`scope_lookup` fallback, recurse instead of declining when the head is either
(a) a builtin whose shape satisfies `rt_builtin_shape_pure` (`elab_fns.c:316`),
or (b) one of the pure control-flow special forms (`if`, `do`, `let`, `let*`,
`letrec`, `while`, `case`, `match`). Both are safe because the recursive descent
at `:1644-1646` still visits their subforms, so a write nested inside one is
still seen. Do **not** extend this to `tur_name_is_reserved_special_form`
wholesale -- that list also contains real write channels (`ptr-write`,
`array-set-unchecked`, `raw-memset`, `raw-free`, `drop!`, `rc/drop`).

## Blast radius, either way

The change is verdict-**widening**, so expect fixtures that currently pin
UNVERIFIED to move (`read-frames-dump-verdicts`, `g1-writes-global-unverified`,
and `wf1-writes-frame-honored`'s `peek`). Each needs reading individually: a
fixture pinning UNVERIFIED *because the walk cannot see a callee* is still
correct, while one pinning it because of a field read or an `if` is pinning this
bug.

Widening also feeds WF3: more frames become `writes_checked`, so more borrow
widening is admitted and more refinement crossings may prove. That is the
intended effect, but it means refinement fixtures are in the blast radius too,
not just write-frame ones.
