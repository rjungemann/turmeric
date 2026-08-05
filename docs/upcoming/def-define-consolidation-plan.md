# Consolidate `def` and `define`

> **Status:** EXECUTED (2026-08-05). D1-D3 landed (§7), D4 landed (§8).
> Nothing in this plan is outstanding.
> **Type:** Language / elaboration

## 0. Summary

Turmeric has two binding forms that differ by *position* rather than by
meaning:

- `def` -- top level only. Hard-errors anywhere else.
- `define` -- body positions only. Has no top-level meaning in a compiled
  file, and only works at the REPL via a special case.

A user who learns one has to learn the other, and the error they get when
they reach for the wrong one tells them a rule rather than doing the obvious
thing.

**Target:** one form, `def`, that means "bind this name here" -- a top-level
binding at the top level, a scoped binding in a body. `define` becomes a
spelling of the same thing. Nothing that compiles today stops compiling.

---

## 1. How the two work now

### 1.1 `def` -- an elaborator form

`elab_def` (`src/compiler/elab_fns.c:7321`), dispatched from
`elab_call.c:1892` on `e->sym_def`. Grammar:

```
(def [^persistent] [^deprecated ["msg"]] name [: type] init)
```

It refuses non-global scope outright:

```c
/* Top-level only — error if not in global scope. */
if (e->scope != &e->global) {
    diag_emit(DIAG_ERROR, call->span, "def is only valid at the top level");
```

and it rejects redefinition of an existing global.

### 1.2 `define` -- a pre-elaboration rewrite

`splice_internal_defines` (`src/compiler/elab_forms.c:82`) is a Form -> Form
pass that turns a body window containing `define`s into nested `let`s:

```
(define x 1)       =>  (let [x 1]      <rest of body>)
(define ^mut x 1)  =>  (let [^mut x 1] <rest of body>)
```

It returns `NULL` when no `define` is present, so bodies without one take the
existing path untouched -- a true no-op, which is exactly the property that
makes extending it safe.

Callers, i.e. the set of positions where `define` is legal today:
`elab_forms.c:182` (recursive tail), `:1029`, `:1683`, `:2085`;
`elab_fns.c:4803`, `:6506`; `elab_macros.c:539`, `:582`, `:636`.

Grammar:

```
(define [^mut|^persistent|^linear|^unique|^affine|^relevant]* name init)
```

### 1.3 The REPL special case

`src/turi/eval.c:10300-10340` scans each new REPL turn for a top-level
`define` and, if it finds one, wraps that turn's forms in a single `(do ...)`
so the splice helper sees a body. The comment says why: *"This makes `define`
usable at the REPL prompt without erroring."*

A consequence worth naming, because §3.4 removes it: because the wrap scopes
the `define` to that turn's `do`, **a name bound with `define` at the REPL
does not survive to the next turn.**

### 1.4 The feature matrix, as it stands

| | `def` (top level) | `define` (body) |
|---|---|---|
| `: type` ascription | yes | **no** -- `(define x : int 1)` is an error |
| `^mut` / `^linear` / `^affine` / `^relevant` / `^unique` | no | yes |
| `^persistent` | yes | parsed, but a `let` binding |
| `^deprecated ["msg"]` | yes | no |
| redefinition | error | shadows |

Neither is a subset of the other, which is the clearest sign these are one
form wearing two hats.

---

## 2. Target semantics

**`def` at the top level: unchanged.** Same grammar, same static storage,
same redefinition error, same `^persistent` / `^deprecated`. This is load
bearing -- it is the form every existing program and every fixture uses.

**`def` in a body: what `define` does today.** Splices into a `let` scoped
over the rest of the body.

**`define`: an alias for `def`, in both positions.** Same grammar, same
diagnostics (with the name the user actually wrote echoed back).

Position, not spelling, selects the behaviour -- which is the rule users
already carry in their heads from Scheme, Clojure, and every other Lisp where
an internal definition is a `letrec` in disguise.

---

## 3. Implementation

### 3.1 Teach the splice to accept `def`

`splice_internal_defines`'s pre-scan (`elab_forms.c:88-100`) and its
per-form match (`:105-108`) compare against `e->sym_define`; both become
`sym_define || sym_def`. That single change makes `def` work in every body
position `define` already works in, because the splice runs *before*
`elab_call` dispatch -- a body-position `(def x 1)` is rewritten to a `let`
and `elab_def` never sees it.

Diagnostics inside the splice currently hardcode the word `define`
("define requires (define name init)"). They take the head symbol's name so
the message quotes what was written.

### 3.2 Teach the top level to accept `define`

`elab_call.c:1892` grows `if (name == e->sym_define) return elab_def(e, call);`
alongside the `sym_def` case, and the top-level dispatcher does the same.
`elab_def` echoes the written spelling in its diagnostics.

### 3.3 The residual "neither top level nor a body window" case

Some positions are neither: an `if` branch, a call argument, a `cond` test.
There, `(def x 1)` still reaches `elab_def` in non-global scope. That must
stay an error -- `(if c (def x 1) ...)` has no sensible meaning, since the
binding would have nowhere to scope over.

The message improves from a rule to an explanation:

```
error: `def` here has nothing to scope over
  a `def` in an expression position binds a name no later form can see.
  Put it at the top level, or in a body (`do`, `fn`, `let`, `when`, `while`),
  or use `let` if you meant a binding local to this expression.
```

### 3.4 Retire the REPL wrap

Once `define` at the top level *is* `def`, `eval.c:10300-10340` has nothing
to work around: a REPL `(define x 1)` becomes a genuine top-level binding.
Delete the wrap.

This is a **behaviour change, and an improvement**: a name defined at the
prompt now survives to the next turn (§1.3). Call it out in the changelog --
someone relying on the turn-scoped behaviour would be relying on a bug, but
they should still read about it.

Also update the `:help` text at `src/turi/repl.c:629`, which currently
describes `def` as top-level-only.

### 3.5 Close the feature gaps (§1.4)

These are the substance of the change; without them "one form" is only half
true.

**(a) `define`/local `def` gain `: type`.** `let` bindings already support
`[name : type init]` (`elab_forms.c:602-620`, accepting both fused `:type`
`F_KEYWORD` and spaced `: type` `F_TYPE_ANN`), so the splice just forwards the
annotation form into the binding vector it builds (`elab_forms.c:164-172`).
Small change, and it removes the most surprising asymmetry in the table.

**(b) Top-level `def` gains the substructural annotations** -- `^mut`,
`^linear`, `^unique`, `^affine`, `^relevant` -- or, where one genuinely has no
top-level meaning, says so specifically. `^mut` on a global is meaningful;
`^linear` on a global is a real question this plan does not answer. **Resolve
per annotation during implementation, and reject with a reason rather than
silently ignoring.** A silently-dropped `^linear` is a worse outcome than an
error.

**(c) `^persistent` and `^deprecated` in a body: explicit errors.**
`^persistent` is static-storage semantics with no local meaning, and
`splice_internal_defines` currently *accepts* the token and produces an
ordinary `let` binding (`elab_forms.c:126-137`) -- i.e. it silently does not
do what it says. `^deprecated` on a local is meaningless. Both become
diagnostics naming the annotation.

**(d) Redefinition stays position-dependent.** Top level errors; a body
shadows, the same as `let`. This is not an inconsistency to fix: at the top
level a duplicate name is almost always a mistake, and in a body shadowing is
the normal, useful thing that `define` already does. Document it rather than
unify it.

---

## 4. Compatibility

- Nothing that compiles today changes meaning. `def` at the top level is
  untouched; `define` in a body is untouched. Every change is a position or
  spelling that is an **error** today becoming legal.
- The one behaviour change is REPL `define` persistence (§3.4).
- No codegen change is expected: local `def` produces the same `let` a local
  `define` produces, and top-level `def` is untouched. **If a fixture
  snapshot moves, that is a signal to investigate, not a snapshot to
  regenerate reflexively.**

### 4.1 Documentation

- CLAUDE.md, the syntax guide, and the tutorials describe one form with two
  positions. `define` is documented as an accepted alias, once, rather than as
  a parallel feature with its own section.
- The REPL help string (§3.4).
- Whether `define` is eventually *deprecated* rather than merely aliased is
  a separate call with its own cost (every doc, tutorial, and example using
  it). **This plan aliases only.** A `^deprecated`-style nudge is a later
  decision made on evidence of which spelling people actually reach for.

---

## 5. Phases

- **D1 -- alias plumbing.** §3.1, §3.2, §3.3. Both spellings work in both
  positions; error message improved. Ships useful on its own.
- **D2 -- REPL.** §3.4. Wrap deleted, help text updated, changelog entry.
- **D3 -- feature parity.** §3.5 (a) and (c) -- the type ascription and the
  two silently-wrong annotation cases. These are the ones that make the
  consolidation real rather than cosmetic.
- **D4 -- annotation audit.** §3.5 (b), one annotation at a time, each with
  either a defined top-level meaning or a specific rejection.

---

## 6. Testing

Fixtures, ASCII only, covering the cells the matrix in §1.4 currently leaves
empty:

- `(def x 1)` in a `do`, an `fn` body, a `let` body, a `when` body, a `while`
  body, and a macro expansion -- one per splice call site (§1.2), because
  those call sites are the actual definition of "body position".
- `(define x 1)` at the top level of a compiled file, and the same at the
  REPL followed by a second turn that reads `x` (the §3.4 change).
- `(define x : int 1)` and `(def x : float 7.1 )` locally -- per CLAUDE.md's
  float rule, the float probe uses `7.1`, never `7.0`, so a truncation bug
  cannot hide.
- `(if c (def x 1) 2)` -> the §3.3 diagnostic, asserted on the message.
- `(define ^persistent x 1)` in a body -> error, not a silent `let`.
- Shadowing: two `def`s of the same name in one body -> second shadows;
  two at the top level -> error.
- Interpreter parity: every one of the above under `tests/run-turi.sh` as
  well as `tests/run.sh`.
- Twelve-minute timeout on every suite run.

---

## 7. Execution record (2026-08-05)

**D1, D2, D3 landed. D4 remains open.** Both suites green after the change:
`bash tests/run.sh` -> 2532 passed, 0 failed; `bash tests/run-turi.sh` ->
1723 passed, 0 failed, 704 skipped. **No fixture snapshot moved** -- §4's
"no codegen change is expected" held, so nothing needed regenerating and
nothing needed investigating.

### 7.1 What landed

**D1 -- alias plumbing.**

- `splice_internal_defines` (`src/compiler/elab_forms.c`) now recognizes both
  spellings through a new `splice_body_def_head` helper, which also returns the
  head symbol so every diagnostic in the splice quotes what the user wrote.
- `elab_call.c` dispatches `sym_def` and `sym_define` to the same `elab_def`.
  `elab_define_error` is deleted (definition and declaration).
- `elab_def` (`src/compiler/elab_fns.c`) reads the head spelling into `kw` and
  echoes it in all five of its diagnostics.
- §3.3's message replaced "def is only valid at the top level".

**D2 -- REPL.** The implicit-do wrap at `src/turi/eval.c` step 5b is deleted;
`src/turi/repl.c`'s `:help` table describes `def` by position and lists
`define` as the alias. Verified by hand: `(define x 41)` then `(+ x 1)` on the
next turn evaluates to `42`, where it was previously unbound.

**D3 -- feature parity.** §3.5(a) type ascription (both the spaced `: type`
`F_TYPE_ANN` and the fused `:type` `F_KEYWORD` forms) and §3.5(c) the
`^persistent` / `^deprecated` rejections.

### 7.2 The scope guard §3.1 did not anticipate

§3.1 proposed making the splice's symbol comparison `sym_define || sym_def`
unconditionally. That would have changed the meaning of a **top-level**
`(do ...)` window: `elab_do` does not push a scope, so a top-level
`(do (def x 1) ...)` creates a global today, and an unconditional splice would
have demoted it to a `let` -- a violation of §4's "nothing that compiles today
changes meaning."

`splice_body_def_head` therefore accepts `def` only when
`e->scope != &e->global`. `define` is unchanged in every position, so the guard
is invisible except in the one corner it protects. A survey of the tree found
**zero** top-level `(do ...)` windows containing a `def`, so this is a
compatibility guarantee rather than a fix for anything in-tree.

### 7.3 A defect found on the way: the dropped prefix

`splice_internal_defines` built its `let` from the first `define` onward and
returned it as the whole body, **discarding every form before it**:

```turmeric
(defn main [] : int
  (println "before")   ;; never ran
  (define x 1)
  (println x)
  0)                   ;; printed only "1"
```

Fixed by wrapping the prefix and the generated `let` in a `(do ...)` when the
first definition is not at index 0. The re-entrant `elab_do` -> splice call on
that wrapper terminates: the prefix items are by construction not definitions,
and the generated form's head is `let`. Fixture: `def-body-prefix-runs`.

### 7.4 An engine divergence found on the way, NOT fixed

The compiled path elaborates a bare top-level *expression*'s subforms in a
pushed scope; the interpreter elaborates them in the global scope. So
`(if true (def x 1) 2)` at file scope gives the §3.3 diagnostic compiled, but
under `--interpret` mints the global and fails one level up on the `if` type
mismatch. Filed as
[docs/reported/turi-toplevel-expr-subforms-elaborate-in-global-scope.md](../reported/turi-toplevel-expr-subforms-elaborate-in-global-scope.md).

This surfaced because the pre-existing `errors/define-bad-position` fixture is
shared by both harnesses. Its probe moved inside a `defn` body, where the two
engines agree, and it still tests exactly what it tested before.

### 7.5 Fixtures

New, per §6: `def-in-body` (one case per splice call site -- `defn`, `fn`,
`let`, `do`, `when`, `while`), `def-in-macro-body`, `define-at-top-level`,
`def-body-type-ascription` (the float probe uses `7.1`),
`def-body-prefix-runs`, `def-body-shadows`,
`errors/def-in-expression-position`, `errors/define-persistent-in-body`,
`errors/def-deprecated-in-body`, `errors/def-redefine-toplevel`.

Changed: `errors/define-bad-position` (§7.4, plus the §3.3 message).

Two §6 items came out differently than written:

- The `when` case does not assert on `(when ...)`'s value. stdlib's `when` is
  `(defmacro when [test body] (if test (do body)))` -- a two-argument macro
  whose `if` has no else, so it yields nil regardless of its body. That is
  unrelated to this plan; the fixture writes through a `^mut` binding instead.
- Interpreter parity is covered by `tests/run-turi.sh` picking up the same
  fixture directories, not by separate fixtures.

### 7.6 What D4 inherited (now closed -- see §8)

§3.5(b) -- top-level `def` gaining (or specifically rejecting) `^mut`,
`^linear`, `^unique`, `^affine`, `^relevant` -- was untouched by D1-D3.
`elab_def` parsed only `^persistent` and `^deprecated`, so a top-level
`(def ^mut x 0)` fell through to the generic arity diagnostic rather than
either working or saying why not. That is exactly the "silently-dropped
annotation is worse than an error" case D4 exists to close.

---

## 8. D4 execution record (2026-08-05)

**D4 landed; §3.5(b) is closed and the plan is fully executed.** Both suites
green: `tests/run.sh` -> 2541 passed, 0 failed; `tests/run-turi.sh` -> 1732
passed, 0 failed, 704 skipped. No fixture snapshot moved.

### 8.1 The resolution, per annotation

`elab_def`'s annotation parsing became a loop that consumes annotations **in
any order** (matching `let`, and fixing `(def ^deprecated "msg" ^mut x 1)`
along the way), with a decision for every annotation rather than a fallthrough.

| Annotation | Verdict |
|---|---|
| `^mut` | **Accepted.** A mutable global -- static storage `set!` may write. |
| `^persistent` | Accepted (unchanged). |
| `^deprecated "msg"` | Accepted (unchanged). |
| `^linear` | Rejected: the obligation is checked at scope exit; a global has none. |
| `^relevant` | Rejected: same reason. |
| `^affine` | Rejected: the check would count elaboration sites, not run-time uses. |
| `^unique` | Rejected: a global is reachable everywhere, so it cannot be unaliased. |
| anything else `^`-led | Rejected: "unknown annotation", listing what `def` accepts. |

**`^mut` was the load-bearing one**, and the motivating evidence was a
dead-end diagnostic already in the tree:

```
$ tur check g.tur
error: set!: 'counter' is immutable; use ^mut at the binding site to allow it
```

...where the binding site was a top-level `def`, which rejected `^mut`. The
advice named the only fix and the fix did not exist. It does now, and
`errors/def-immutable-global-set` pins the diagnostic so the pair stays honest.

**The four substructural rejections are decisions, not deferrals.** §3.5(b)
said to reject "with a reason rather than silently ignoring", and the reason
differs per annotation:

- `^linear` / `^relevant` are verified at **scope exit** -- elab_let's ST1 pass
  asks "was this consumed / used by the time its scope ended". The global scope
  has no exit, so the obligation has no point at which it could be discharged
  or reported.
- `^affine` is checked **per use site**, in `elab_toplevel.c`'s symbol
  resolution -- which *would* fire on a global, and that is precisely the
  problem. It counts elaboration sites across the whole program, not uses at
  run time, so two functions naming the global would be rejected even if only
  one is ever called. Half-working is worse than absent.
- `^unique` asserts no aliasing. A global is a name every function in the
  program can reach, so uniqueness is not a property it can have.

Each message names the annotation, gives that reason, and points at the body
binding where the annotation does work.

### 8.2 Correction to §7.6

The note left for D4 said §7.2's scope guard "is what currently keeps a
top-level `(do (define ^mut counter 0) ...)` working (fixture
`define-annot`)". That was wrong: the guard conditions only the `def`
spelling, and `define` splices at every scope, so `define-annot` never depended
on it.

The underlying question the note meant to raise -- should the splice become
unconditional now that top-level `def` has a real `^mut`? -- resolves **no**.
The guard exists so a top-level `(do (def x 1) ...)` keeps creating a global
rather than being demoted to a `let` (§7.2), and `^mut` support does not bear
on that. The guard stays.

What D4 *does* change is that the annotation is no longer a reason to prefer
one position: `(def ^mut counter 0)` now works at the top level and in a body,
so `define-annot`'s shape would compile either way.

### 8.3 Fixtures

New: `def-mut-global` (int, float via `7.1`, and cstr globals, mutated from
another function and from `main`), `def-mut-global-annotation-order`,
`def-mut-global-in-module` (a `^mut` global through the module name mangling),
`errors/def-linear-global`, `errors/def-affine-global`,
`errors/def-relevant-global`, `errors/def-unique-global`,
`errors/def-unknown-annotation`, `errors/def-immutable-global-set`.

Verified by hand beyond the fixtures: `set!` on a `^mut` global struct field,
and `(def ^mut n 1)` / `(set! n 41)` / `(+ n 1)` across three REPL turns.

### 8.4 Not in scope, deliberately

A `^mut` global is process-wide mutable state with no synchronization. Nothing
here makes it thread-safe, and no diagnostic warns about sharing one across
threads. That is the same situation every `defopaque` handle and every
`^persistent` global is already in, and giving mutable globals a concurrency
story is its own plan, not a rider on this one.
