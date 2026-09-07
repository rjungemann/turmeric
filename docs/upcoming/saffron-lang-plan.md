# Saffron -- a dynamically typed `#lang` over the Turmeric runtime

Status: **plan only.** Nothing is implemented; there is no `EXPERIMENTS[]` row,
no `#lang` base, no fixture. Every "today" claim below was measured against
`v0.44.2` (`2da89e84`) with the probe transcript in the appendix.

---

## 0. The ask, and the honest size of it

> Saffron is Turmeric without type annotations. Essentially a
> non-static-typed version of Turmeric.

This is several epics, and the request came in knowing that. The value is
real: the runtime underneath is far more capable than what a dynamic Lisp in
this weight class normally gets -- delimited continuations (`shift`/`reset`,
serial and cloneable), algebraic effects with deep and shallow handlers,
generators, STM, channels, a HAMT, a tree-walking interpreter with a debugger
and a DAP server, an incremental REPL, and a macro system with reader macros
and four surface syntaxes. Fennel gets Lua's runtime. Saffron would get this
one.

The load-bearing static machinery -- refinements, session types, linearity,
regions, monomorphization -- does not survive the move, and this plan says so
explicitly rather than pretending it can be retrofitted. See
[Section 6](#6-carve-outs).

---

## 1. Thesis

**Saffron is not a second language. It is Turmeric elaborated with `any` as the
default type instead of `int`, plus the dynamic operator layer that turns `any`
from a storage type into an operational one.**

Same reader. Same macro expander. Same `Expr` tree. Same two back ends. Same
runtime. Same stdlib where the stdlib is polymorphic enough to be reachable.
The delta is concentrated in two places:

1. **Elaboration defaults.** Four sites in `elab_fns.c` currently write
   `TY_INT` for an unannotated parameter or return. Under Saffron they write
   `TY_ANY`.
2. **The dynamic surface.** `any` today can be *stored*, *reflected on*
   (`type-of`, `is?`, `cast`) and *narrowed* in an `if` guard. It cannot be
   added, printed, called, matched, compared, indexed, or field-accessed. That
   list is the actual work.

Everything else -- the `#lang` plumbing, the prelude, the tooling -- is
mechanical by comparison.

Keeping this framing is what bounds the maintenance burden. The moment Saffron
becomes a fork of the elaborator, it stops being affordable.

---

## 2. What already ships (measured)

This section exists because the starting position is much better than it
looks, and the plan's staging depends on knowing exactly how much better.

### 2.1 `any` is a real type with a real runtime representation

`TY_ANY` is the top type (`types.c:5381` -- `if (super_.kind == TY_ANY) return
true;`). Its C representation is `tur_tagged_t { int64_t tag; int64_t val; }`
(`types.c:519`). A value widens into it through `elab_coerce_to_any`
(`elab_call.c:418`), which wraps it in `EX_UNION_INJECT` carrying a runtime
tag. Immediates ride the `val` word; floats ride as their IEEE-754 bit
pattern; by-value aggregates are heap-boxed and the box is freed by
`__tur_any_drop` (`emit_module.c:818`).

Struct and ADT payloads do **not** tag as a bare `TY_STRUCT` -- `emit_any_type_id`
(`emit_module.c:752`) interns the monomorph's `type_name` and hands out
`TUR_ANY_ID_BASE + index`, so `Point` and `Other` are distinguishable at
runtime and `type-of` reports the source-level name.

### 2.2 The reflection surface works

Probed and passing today:

```turmeric
(defn box-str [] : any "hi")
(defn describe [x : any] : cstr
  (if (is? x int) "int"
    (if (is? x cstr) "cstr" "other")))
(defn bump [x : any] : int
  (if (is? x int) (+ x 1) 0))     ;; if-guard narrowing: x is int inside
```

`type-of`, `cast` (checked, panics on mismatch), `is?`, and `if`-guard
narrowing all work on both back ends.

### 2.3 The interpreter is already a dynamic runtime

This is the single most important fact in the plan.

`TuriValue` (`turi/value.h:53`) is a tagged union -- exactly the representation
a dynamic language wants. `+` dispatches on that tag at runtime
(`eval.c:3567`: `bool is_float = (args[0].tag == TURI_FLOAT);`). `println`
dispatches on it too (`eval.c:3759`). And `EX_UNION_INJECT` -- the widen-to-any
node -- is a **no-op** in the interpreter:

```c
/* eval.c:10750 */
case EX_UNION_INJECT:
    return eval_expr(env, frame, e->as.union_inject_.value);
```

because `any` does not need a box when every value is already tagged.

So the interpreter does not need a dynamic runtime built for it. It *is* one.
The only thing standing between `#lang saffron` and a working interpreted
Saffron is the **shared elaborator**, which the parity guide is explicit about:
"The shared front-end (parser, macro expander, elaborator, substructural
checker) runs identically on both paths, so type errors ... are the same."

This is what makes an interpreter-first staging honest rather than a dodge.

### 2.4 Local inference already exists

`let` bindings infer. `(let [x 7.1 y (* x 2.0)] (println y))` prints `14.2`
today -- no annotation anywhere. So Saffron does not need an inference engine
built from nothing; it needs the existing one to stop being cut off at the
`defn` boundary.

There is even a precedent for inferring an unannotated *parameter*:
`elab_fns.c:9569` types an unannotated `fn` param from the expected function
type pushed by the call site, instead of defaulting to `int`. Saffron widens
that seam; it does not open a new one.

### 2.5 The control-flow runtime is type-agnostic

Delimited continuations, effect handlers (deep and shallow), generators, STM,
channels, dynamic variables, panics: none of them are typed machinery in any
way Saffron disturbs. The parity guide marks every one of them `OK`/`OK`. They
should come across for free, and the plan budgets nothing for them beyond
fixtures that prove it.

---

## 3. What does not ship (measured)

Every row below was reproduced against `v0.44.2`. Transcript in the appendix.

| # | Saffron needs | Today | Diagnostic |
|---|---|---|---|
| G1 | unannotated param is dynamic | defaults to `int`; `(twice 7.1)` on `(defn twice [x] (* x 2))` is rejected | TUR-E0001 `expected int, got float` |
| G2 | unannotated return is dynamic | inconsistent: `int` body OK, `cstr` body OK, **`7.1` body is a hard error** claiming the fn "declares return type 'nil'" | TUR-E0707 |
| G3 | `(+ x 1)` where `x : any` | rejected | TUR-E0006 `operator lookup failed for '+', first arg type any` |
| G4 | `(println x)` where `x : any` | rejected -- the union guide's own headline example does not compile | TUR-E0006 |
| G5 | `(f x)` where `f : any` | rejected | `'f' is not a function or continuation` |
| G6 | `(match x)` where `x : any` | rejected at elaboration, though `emit_expr.c:14356` already has a `TY_ANY` scrutinee arm | `match: scrutinee must be an ADT type, got any` |
| G7 | heterogeneous containers | element type unifies; `(vec-of 1 "two")` is rejected | TUR-E0001 |
| G8 | `type-of` on a boxed closure | compiled says `"unknown"`; the interpreter says `"fn"` | -- (silent divergence) |
| G9 | `=`/`<`/`>` on `any` | rejected | TUR-E0006 |
| G10 | `any` truthiness in `if` | untested; `if` wants `bool` | -- |
| G11 | field access on `any` | no dynamic path | -- |

G3, G4, G5, G6, G9 are one problem wearing five hats: **the builtin operator
table is keyed on concrete argument kinds and has no `any` row.** That is the
core engineering work of this plan, and it is the same work on both back ends
-- except that on the interpreter the *implementation* already exists
(Section 2.3) and only the elaborator needs to stop rejecting the call.

G2 and G8 are defects independent of Saffron and are filed separately:
`docs/reported/inferred-return-defaults-inconsistently.md`,
`docs/reported/type-of-on-boxed-closure-diverges.md`. G4's documentation half
is `docs/reported/any-type-guide-examples-do-not-compile.md`.

---

## 4. Design decisions

### D1 -- `#lang` gets a language axis, orthogonal to the reader axis

**Verdict: add `LangDialect` beside `ReaderType`. Do not grow `ReaderType`.**

`#lang <base>[/<dialect>] <layer>*` today resolves the whole first token to a
`ReaderType` (`reader.c:4532`, `lang_base_from_name`). That enum means "which
reader": `turmeric`, `turmeric/curly-infix`, `turmeric/neoteric`,
`turmeric/sweet`.

Saffron is not a reader change. Saffron files want *all four* readers --
`#lang saffron`, `#lang saffron/sweet`, and so on. Folding those into
`ReaderType` would double the enum, and every `switch (reader_type)` in the
tree would then have to remember that half its cases mean the same reader.

So:

```c
typedef enum LangDialect {
    LANG_TURMERIC = 0,   /* the default; every existing file */
    LANG_SAFFRON,
} LangDialect;
```

`lang_base_from_name` splits into a `(LangDialect, ReaderType)` pair.
`detect_lang_layered` gains a `LangDialect *out_lang`. `SourceFile`
(`diag.h:437`) grows a `lang` field beside `reader_type`. `detect_lang` keeps
its signature and discards the new out-param, exactly as it already discards
the layer set -- so no existing caller changes.

Layers stay orthogonal to both axes: `#lang saffron/sweet stringed` is legal
and means what it reads as.

Extensions follow later, not first: `.saf` and `.saf.sweet` in
`reader_type_from_extension`'s sibling, once the semantics are real. Stage 1
is `#lang saffron` inside an ordinary `.tur` file, which keeps every tool
working while the semantics are in flux.

### D2 -- annotations stay legal in Saffron

**Verdict: Saffron is "annotations optional", never "annotations forbidden".**

This is the decision the rest of the design hangs off. It buys three things
that are otherwise very expensive:

- **FFI survives.** `extern-c` and inline-C need concrete C types. A Saffron
  file can annotate exactly the boundary defn and leave the rest dynamic.
- **Migration is bidirectional.** Adding an annotation to a Saffron file is a
  refinement, not a rewrite; deleting one from a Turmeric file is a
  loosening. The two dialects are the same language at different points on
  one dial.
- **Performance has an escape hatch.** A hot Saffron function can be
  annotated and get the static path's codegen with no dialect change.

The corollary is that CLAUDE.md's "No Lazy `:int` Stand-Ins" rule applies to
Saffron code that *does* annotate, unchanged. An unannotated Saffron parameter
is `any`, which is honest; an annotated one that says `:int` for a request
handle is the same defect it always was.

### D3 -- the default type is `any`, and inference still runs

**Verdict: unannotated means `any` as the *declared* type, but local inference
is still allowed to prove something narrower.**

The naive reading of "no static types" is "make everything `any` and box
everything". That would be correct and unbearably slow -- every integer add
would go through a tagged dispatch.

Instead: the *signature* is `any` (that is what makes the call site dynamic and
the language usable), but inside a body, a binding whose value is provably one
concrete type stays that type. `(let [x 7.1] (* x 2.0))` should remain a
`double` multiply in a Saffron file, exactly as it is today. The machinery for
this already exists -- it is what makes G2's `cstr` case work.

This gives Saffron a clean optimisation ladder that does not need to be built
up front:

1. **S3 baseline** -- every `any` operation goes through dynamic dispatch.
2. **Later** -- local type propagation un-boxes within a body.
3. **Later still** -- a whole-program pass that observes a Saffron defn is
   only ever called at one argument type and specialises it.

Rung 1 is the plan. Rungs 2 and 3 are explicitly out of scope and named here
only so the design does not foreclose them.

### D4 -- the dynamic surface is eight operations

Saffron needs `any` to support exactly these, and this list should not grow
without a decision:

| Operation | Compiled lowering | Interpreter |
|---|---|---|
| arithmetic (`+ - * / mod`) | `__tur_dyn_arith(op, tur_tagged_t, tur_tagged_t)` in the preamble | exists (`eval.c:3567`) |
| comparison (`= < > <= >= !=`) | `__tur_dyn_cmp` | exists (`eval.c:3618`) |
| print (`println`, `print`, `str`) | `__tur_dyn_print`, reusing `__tur_any_name_ext` | exists (`eval.c:3759`) |
| call (`(f x ...)`, `f : any`) | unbox to fat pointer, check arity, indirect call | closure values already first-class |
| truthiness (`if`, `when`, `and`, `or`) | `__tur_dyn_truthy` | tag test |
| field access (`(.f x)`, `(set! (.f x) v)`) | per-type field table keyed on the box id | `TuriStruct` field lookup exists |
| type dispatch (`match` on `any`) | box-id switch; `emit_expr.c:14356` is half-plumbed | tag switch |
| index (`(vec-get v i)` etc. on `any`) | unbox then delegate | delegate |

**The float rule is load-bearing here.** `tur_tagged_t.val` is `int64_t` and a
float rides as its IEEE-754 bit pattern. Every dynamic arithmetic path must
branch on the tag and *reinterpret*, not convert. Per CLAUDE.md, every probe
in this area leads with `7.1`, never `7.0` and never `7` -- an integer literal
cannot show the truncation this would produce if the reinterpret is wrong.
`tests/fixtures/saffron-dyn-arith-float` exists to pin exactly that.

**Truthiness needs a decision, and this plan makes it:** in Saffron, `false`
and `nil` are falsy; **`0`, `""`, and the empty container are truthy.** That is
the Lisp/Clojure convention rather than the C/Lua one, it matches the fact that
Turmeric's `if` already wants a `bool` (so there is no legacy int-truthiness to
preserve), and it avoids the classic `(if (vec-len v) ...)` bug.

### D5 -- the Saffron/Turmeric boundary is a contract boundary

**Verdict: gradual typing with runtime checks at the seam, not erasure.**

A Turmeric module importing a Saffron module sees `any`-typed exports and must
narrow them (`cast`, `is?`, `match`) like any other `any`. That is already how
`any` works; nothing new is needed.

A Saffron module importing a Turmeric module is the interesting direction. The
callee has a real signature. Three options were considered:

- **(a) Reject** unless the Saffron caller annotates. Safe, and unusable.
- **(b) Erase** -- pass the `any` payload through unchecked. Fast, and it turns
  a type error into a memory-safety bug, because the payload word is
  reinterpreted at whatever the callee declared.
- **(c) Check at the seam** -- insert an implicit `cast` at each argument whose
  static type is `any` and whose parameter type is concrete. A mismatch panics
  with the existing `cast: any holds cstr, not int` message.

**(c).** It is the only option that keeps the typed half of the program's
guarantees actually meaning something, the check is one tag compare, and the
machinery (`EX_ANY_CAST`, `__tur_any_cast_check`) already ships. This is the
standard sound-gradual-typing answer and the reason `cast` was built checked
rather than unchecked in the first place.

The cost is honest and should be documented rather than hidden: a Saffron
program that calls the typed stdlib in a hot loop pays one tag compare per
argument. `--saffron-unchecked-boundary` is deliberately **not** proposed; if
the cost ever matters, the answer is D3's rung 2, not a soundness switch.

### D6 -- refinements become runtime contracts

**Verdict: a refinement in a Saffron file lowers to a runtime contract check,
not an error and not a silent drop.**

`#refine{x : int | (> x 0)}` needs a static base type and an SMT discharge.
Neither exists over `any`. But the language already ships the dynamic answer:
`stdlib/contract.tur`, with `assert!`/`require!`/`ensure!` and a contract
handler.

So a refinement on a Saffron binding keeps its predicate and checks it at
runtime. This is a genuinely good outcome rather than a consolation prize --
it is exactly the design Racket landed on, the predicate stays in the source
where a reader can see it, and a Saffron file that later gets annotated can
have its contracts discharged statically without changing a character.

The one thing that must not happen is a refinement being accepted and
**ignored**. That would make a Saffron file look like it carries a guarantee
it does not have.

### D7 -- static-only features are rejected, with a message that says why

**Verdict: hard error naming the feature and the reason, never a silent
downgrade.**

In a Saffron file, these are `TUR-E03xx` errors:

- session types (`Session[P]`, `make-session`) -- the protocol *is* the type
- GADTs (`defgadt`) -- index refinement is the whole feature
- linear / affine / unique (`^linear`, `^unique`, `lref<T>`)
- explicit borrows (`&T`, `&mut T`) and lifetime annotations
- `with-region` / region brackets

The last three deserve a note. Regions rewind a generation only when the
compiler can *prove* nothing outside it points in, and that proof is a static
walk over the bracket's result type (see the Region Store Hooks rule in
CLAUDE.md). Over `any`, the result type says nothing. A region bracket in a
Saffron file would be a use-after-rewind waiting to happen, which is precisely
the failure mode `docs/reported/region-escape-through-unhooked-stores.md`
documents. **Saffron runs on the GC/RC arm; regions are off, not degraded.**

Note this is a *file*-level restriction, not a program-level one. A Turmeric
module in the same project keeps regions, linearity, and sessions. The two
dialects link together; they just do not each get the other's guarantees.

### D8 -- typeclasses need a static receiver, at first

**Verdict: S1-S7 require a statically known receiver type at each typeclass
method call. Runtime instance dispatch is a separate epic, deliberately not
scheduled.**

Dictionary resolution is by static type. Doing it dynamically means a runtime
instance table keyed on the box id -- which is *tractable*, because
`emit_any_type_id`'s interned name table is already exactly that registry. But
it interacts with superclasses, default methods, HKT receivers, and
monomorphization, and it is the single largest item that could be attached to
this plan.

It is therefore not attached. Saffron ships without runtime typeclass
dispatch, `(show x)` on an `any` is an error with a message pointing at
`cast`, and the decision to build it is taken later on evidence.

### D9 -- the gate

Per CLAUDE.md's Experimental Compiler Features rule, Saffron is exactly the
shape that must ship behind `--enable=`:

```c
/* src/runtime/experiments.c -- the first live row since option-niche. */
{ "saffron",
  "dynamically typed dialect (#lang saffron)",
  "docs/upcoming/saffron-lang-plan.md",
  "0.45.0",                   /* introduced */
  "0.52.0",                   /* expires_at -- advisory, never blocks a cut */
  XF_LIFECYCLE_PROTOTYPE,
  &g_opt_saffron },
```

`#lang saffron` is the file-scoped enable, at CLI precedence -- the same
relationship a semantic `#lang` layer has to its experiment. It therefore
inherits that rule's sharp edge, and should: a project manifest that scopes
`:experiments` and leaves `saffron` out makes `#lang saffron` a **hard error**,
not a silent ignore. `lang_layers_apply_semantic` already implements exactly
this policy (`lang_layers.c:160`); the dialect axis calls the same code.

`expires_at` is a deadline, not an earliest date. Graduating early is routine
and an expiring row never blocks a release cut.

---

## 5. Stages

Each stage is independently landable and leaves the tree green-ish. Sizes are
rough multiples of a day of focused work, not commitments.

### S0 -- prerequisites (small)

Fix the three `any` defects that are wrong regardless of Saffron, so the
dialect is not built on them:

- G2: unannotated return defaults inconsistently, and a float body reports
  "declares return type 'nil'" -- a message that names a declaration the
  programmer did not write.
- G8: `type-of` on a boxed closure -- compiled `"unknown"` vs interpreted
  `"fn"`.
- G4-docs: the union/intersection guide's headline `any` examples do not
  compile (`: unit` is not a type; `println` has no `any` overload).

**Open question to settle here, with a recipe:** `emit_any_type_id` interns
per-`EmitCtx`, and `EmitCtx` is per-TU (`emit_module.c:13604`, `:16145`), so
`TUR_ANY_ID_BASE + index` is assigned in first-seen order *within one C file*.
In Saffron every value is `any` and crosses module boundaries constantly, so
these ids must be stable across TUs. A two-module probe with deliberately
different intern orders answered correctly (appendix A.3), which means either
the ids happen to agree or something normalises them -- and "happens to agree"
is not a foundation. **Recipe:** emit a project build with
`--build-dir <keep>`, grep each `obj/*.c` for its `__tur_any_name_ext` switch,
and compare the id assigned to a shared type. If they diverge, S0 grows a
deterministic global id (hash of `type_name`, or a link-time table) before
anything else lands.

**Exit:** three reports archived, the id question answered in writing.

### S1 -- the `#lang` axis, no semantics (small)

D1's plumbing only. `LangDialect`, `SourceFile.lang`, `detect_lang_layered`
out-param, `lang_base_from_name` returning a pair, `reader_type_name`'s
sibling `lang_dialect_name`. The `EXPERIMENTS[]` row from D9, with
`g_opt_saffron` read by nothing yet.

`#lang saffron` compiles a file **exactly as `#lang turmeric` does** and warns
once (TUR-W0060, the experiment lifecycle warning) that the dialect carries no
semantics yet.

**Exit:** `#lang saffron` and `#lang saffron/sweet` parse; `tur lang-layers`
grows a dialect listing; `tests/fixtures/saffron-lang-line-accepted`,
`errors/saffron-without-enable`.

### S2 -- default to `any`, interpreter first (medium)

Flip the four `TY_INT` defaults in `elab_fns.c` (defn params ~6726, fn params
~9566, defn return ~7183, fn return ~9705) to `TY_ANY` **when the enclosing
`SourceFile.lang` is `LANG_SAFFRON`**. Keep D3's local inference.

At this point almost every interesting Saffron program still fails, because
the operators reject `any`. That is fine and expected: S2's job is to prove
the default flips cleanly and that a program which only *moves* dynamic values
around (binds them, passes them, returns them, stores them) works end to end
on both back ends.

**Exit:** `(defn id [x] x)` called at `int`, `cstr`, and `7.1` from one Saffron
program, compiled and interpreted, same output.

### S3 -- the dynamic operator layer, interpreter (medium)

The elaborator learns to route a builtin call with an `any` argument to a
dynamic node (`EX_DYN_OP`) instead of failing operator lookup. The
interpreter's arm for that node is thin, because `eval.c:3567` and `:3759`
already do the work.

Covers G3, G4, G9, and D4's truthiness rule.

**Exit:** `(defn twice [x] (* x 2))` called with `21` and with `7.1` in one
interpreted program, printing `42` and `14.2`.

### S4 -- dynamic call and dynamic field access, interpreter (medium)

`EX_DYN_CALL` (G5) and dynamic `(.f x)` (G11), plus `match` on an `any`
scrutinee (G6). Closures widen into `any` with a real tag, which also closes
G8 properly rather than papering it.

This is the stage that makes Saffron a *language* rather than a calculator --
higher-order functions are the whole point of the surface syntax.

**Exit:** `map`/`filter`/`fold` over a heterogeneous list, written in Saffron,
interpreted.

### S5 -- the compiled path (large)

Everything S3 and S4 did for the interpreter, done again in the C preamble:
`__tur_dyn_arith`, `__tur_dyn_cmp`, `__tur_dyn_print`, `__tur_dyn_truthy`,
`__tur_dyn_call`, the field table, the `match`-on-`any` box-id switch (half of
which `emit_expr.c:14356` already sketches).

This is the biggest single stage and the one where the float reinterpret
(D4) and the box-ownership question (below) both bite.

**Ownership is the real risk here, not the dispatch.** `any` boxing already has
a known ownerless-box problem in the union case
(`docs/reported/union-tagged-union-c-emission.md`), and Saffron multiplies
every widen site by roughly the size of the program. The mitigation is to make
Saffron's `any` boxes RC-managed rather than raw `malloc`/`__tur_any_drop`, and
to pin it with `tests/run-leak-check.sh` from the first fixture rather than
retrofitting. Budget for this explicitly; it is where a "just box everything"
design usually goes wrong.

**Exit:** every S3/S4 fixture passes compiled with identical output, and the
Saffron fixtures carry `requires.leak-check`.

### S6 -- containers and the Saffron prelude (medium)

G7. `(vec any)` becomes the default container element in Saffron, so
`[1 "two" 7.1]` is a vector of three boxes. Same for `#map{...}`, `#set{...}`,
and cons lists.

A `stdlib/saffron/prelude.tur` autoloads for `LANG_SAFFRON` files: the dynamic
`map`/`filter`/`fold`/`reduce`/`assoc`/`get`, the truthiness helpers, and thin
`any`-taking wrappers over the typed stdlib entry points a dynamic program
reaches for constantly. This is where Saffron stops feeling like Turmeric with
the types removed and starts feeling like its own thing.

### S7 -- the boundary (medium)

D5's implicit checked `cast` at each Saffron -> Turmeric argument whose
parameter is concrete and whose argument is `any`. Fixtures for both
directions, including the panic path.

### S8 -- tooling (medium, parallelisable)

`tur fmt` / `tur format` (the formatter reads `reader_type`; it needs to
preserve a `#lang saffron` line and not add annotations), `tur lsp` and
`lsp-lite` (a Saffron file must not report `any` everywhere as an error),
`tur repl --lang saffron`, `tur init --saffron`, `tools/gendocs.py`
(docstrings without types), the vim/vscode syntax packs, and a
`docs/guides/saffron-guide.md`.

---

## 6. Carve-outs

Permanent. These are not "later" -- they are the honest answer to "what does
Saffron give up".

| Feature | Status in Saffron | Why |
|---|---|---|
| Refinement types | runtime contracts (D6) | no static base type to discharge over |
| Session types | rejected | the protocol is the type |
| GADTs | rejected | index refinement is the whole feature |
| Linear / affine / unique | rejected | static use-count proof |
| Explicit borrows, lifetimes | rejected | static region proof |
| `with-region` / regions | off; GC/RC arm | the escape proof is a walk over the result type (D7) |
| Monomorphization, by-value HKT | off; everything boxes | requires ground types at each site |
| Typeclass dispatch on `any` | rejected, for now (D8) | separate epic |
| `extern-c`, inline-C | requires annotations (D2) | C needs concrete types |
| Effect rows, `--strict-effects` | inferred as today | effect rows are not value types; expected to work unchanged |

---

## 7. Risks

**R1 -- the elaborator forks anyway.** Every stage that adds
`if (lang == LANG_SAFFRON)` to a hot path in `elab_fns.c` / `elab_call.c` is a
step toward two elaborators. Mitigation: make the dialect flip *data* (a
default `TypeKind`, a table row) rather than *control flow*, wherever the shape
allows.

**R2 -- boxing costs, discovered late.** A dynamic program in a system with no
generational GC and no NaN-boxing allocates hard. S5 pins the leak story from
the first fixture, but throughput is unmeasured. Mitigation: a Saffron row in
`benchmarks/` from S3 onward, not from S8.

**R3 -- fixture count.** The suite is ~1442 fixtures and ~4-5 minutes. Saffron
needs each behaviour proven on both back ends, so its fixtures roughly double
per feature. Keep them small and prefer one fixture per behaviour over one per
combination.

**R4 -- `expires_at` misread as a gate.** CLAUDE.md is explicit that an
expiring `EXPERIMENTS[]` row never blocks a release cut, and that believing
otherwise has already stranded two releases. Saffron will be the only live row
for a while, which makes it the most likely candidate to be misread that way.

**R5 -- scope creep into a second stdlib.** S6's prelude is the thin end of a
wedge that ends with a parallel dynamic stdlib. The prelude should be
adaptors, not reimplementations, and any file added to it needs a reason
beyond "the typed one needs annotations".

---

## 8. Open questions

1. **Cross-TU `any` id stability** -- S0's recipe answers it. Everything
   downstream assumes the answer is yes.
2. **`.saf` extension** -- worth it, but not before the semantics settle;
   sequencing it early means every tool learns a file type whose meaning is
   still moving.
3. **Does Saffron get its own `main` convention?** `(defn main [] : int ...)`
   is awkward in a file with no annotations. A Saffron `main` returning `nil`
   and exiting 0 is the obvious answer and costs almost nothing; deferred to
   S6 so it lands with the prelude.
4. **REPL default dialect.** `tur repl` is the surface where a dynamic dialect
   is most valuable and where changing the default is most disruptive. Propose
   `--lang saffron` opt-in at S8; revisit after there is usage.
5. **Should the interpreter ever be Saffron-only?** If the compiled path (S5)
   proves disproportionately expensive, an honest fallback is "Saffron runs on
   the interpreter and the JIT, not on `cc`". That is a real product, given the
   REPL and DAP already exist. Recorded so the option is on the table rather
   than discovered under pressure.

---

## Appendix A -- probe transcript

All against `./build/tur` at `v0.44.2` (`2da89e84`), Debug build.

### A.1 -- the `int` default (G1, G2)

```
$ cat p1.tur
(defn add1 [x] (+ x 1))
(defn main [] : int (println (add1 41)) 0)
$ tur run p1.tur
42

$ cat p2.tur
(defn scale [x] (* x 2.0))
(defn main [] : int (println (scale 7.1)) 0)
$ tur run p2.tur
error [TUR-E0042]: mixed-width numeric arithmetic: '*' arg 2 is float, expected int

$ cat p17.tur
(defn twice [x] (* x 2))
(defn main [] : int (println (twice 21)) (println (twice 7.1)) 0)
$ tur run p17.tur
error [TUR-E0001]: function 'twice' arg 1: expected int, got float

$ echo '(defn greet [] "hello")' ...        =>  prints "hello"   (return inferred cstr)
$ echo '(defn n [] 42)' ...                 =>  prints 42        (return inferred int)
$ echo '(defn pi [] 3.14)' ...              =>  error [TUR-E0707]: function 'pi' declares
                                                return type 'nil' but its body returns float
```

The third line is G2: three unannotated returns, three different behaviours,
and an error message naming a declaration the programmer never wrote.

### A.2 -- `any` is storage-only (G3-G6, G8)

```
$ cat p3.tur
(defn bump [x : any] : any (+ x 1))
$ tur run p3.tur
error [TUR-E0006]: operator lookup failed for '+': got 2 arg(s), first arg type any
  note: available overload: + arity 2..* arg=int result=int
  note: available overload: + arity 2..* arg=float result=float

$ cat p7.tur                       # the union guide's headline example, corrected to : nil
(defn debug-print [x : any] : nil (println x))
$ tur run p7.tur
error [TUR-E0006]: operator lookup failed for 'println': got 1 arg(s), first arg type any

$ cat p8.tur
(defn get-any [] : any (make-struct Point 3 4))
... (match a (n : int) ... (p : Point) ...)
$ tur run p8.tur
error: match: scrutinee must be an ADT type, got any

$ cat p9.tur
(defn apply2 [f x] (f x))
$ tur run p9.tur
error: 'f' is not a function or continuation

$ cat p18.tur
(defn mk [] : any (fn [x : int] : int (+ x 1)))
(defn main [] : int (println (type-of (mk))) 0)
$ tur run p18.tur
unknown                            # the interpreter answers "fn" (eval.c:10810)
```

What *does* work (p5.tur, unmodified):

```
$ tur run p5.tur
int                                # (describe (box-int))  -- is? narrowing
cstr                               # (describe (box-str))
42                                 # (bump (box-int))      -- narrowed arithmetic
cstr                               # (type-of (box-str))
hi                                 # (cast (box-str) cstr)
```

And local inference, unannotated, with a real fractional part:

```
$ cat p10.tur
(defn main [] : int (let [x 7.1 y (* x 2.0)] (println y)) 0)
$ tur run p10.tur
14.2
```

### A.3 -- cross-TU `any` ids (S0's open question)

Two-module project, `amod` widening a `Beta` to `any`, `main-mod` interning
`Gamma` *first* so its local id ordering differs from `amod`'s:

```
$ tur build . && ./build/bin/anyids
Gamma
Beta
1
```

Correct. But `EmitCtx` is per-TU and ids are `TUR_ANY_ID_BASE + first-seen
index`, so this needs the S0 recipe to explain *why* it is correct before
Saffron depends on it.

### A.4 -- `#lang` today

```
$ printf '#lang saffron\n...' > p12.tur && tur run p12.tur
tur: error: #lang unknown is not yet implemented

$ tur lang-layers
NAME         KIND      SINCE   SUMMARY
stringed     reader    v1      #s"..." owned-String literal (string/from-cstr)
```

---

## See also

- [docs/archive/lang-layers-plan.md](../archive/lang-layers-plan.md) -- the
  `#lang` base/layer model D1 extends
- [docs/guides/union-intersection-types-guide.md](../guides/union-intersection-types-guide.md)
  -- `any`, `cast`, `type-of`, `is?`, `if`-guard narrowing
- [docs/guides/turi-parity-guide.md](../guides/turi-parity-guide.md) -- what
  the interpreter can already run
- [docs/guides/experimental-flags-guide.md](../guides/experimental-flags-guide.md)
  -- the `--enable=` lifecycle D9 uses
- [docs/guides/contract-types-guide.md](../guides/contract-types-guide.md) --
  the runtime contracts D6 lowers refinements to
