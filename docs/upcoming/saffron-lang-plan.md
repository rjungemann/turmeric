# Saffron -- a dynamically typed `#lang` over the Turmeric runtime

Status: **plan only for Saffron itself** -- there is no `EXPERIMENTS[]` row, no
`#lang` base, no dialect fixture.

But S0's prerequisites are largely burned down. Fixed and archived as of
2026-09-07: **P1** (cross-TU `any` ids), **P2** (by-value rank-2 receiver,
guarded), **P2b** (`@TypeName` implies the unbox), **P2c** (parametric
narrowing), **P2d** (monomorph under a widen), **P3** (unannotated return
inference), **P6** (`: any` drives coercion at `let`/`def`/`if`/letrec), and
**P8** (float truncated through the dict carrier). Remaining: P4, P5, P7 --
all cosmetic or docs.

Worth stating plainly, because it changes how the rest of this plan should be
read: **six of those eight reports had a diagnosis that was wrong on
inspection**, always in the same direction -- control flow read and a cause
inferred, rather than measured. P2d and P3 were ordering bugs, not missing
machinery. P8's recommended direction was the larger one. P6 had four causes,
not the two filed. The "today" claims below were written the same way, so treat
them as leads to verify rather than findings, and instrument before acting on
any of them.

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
| ~~G2~~ | unannotated return is dynamic | **FIXED 2026-09-07** -- was inconsistent (`int`/`cstr` bodies OK, `7.1` a hard error claiming the fn "declares return type 'nil'"). Now inferred for every body type. Still `int`-shaped rather than dynamic, which is S2's job, but no longer wrong | -- |
| G3 | `(+ x 1)` where `x : any` | rejected | TUR-E0006 `operator lookup failed for '+', first arg type any` |
| G4 | `(println x)` where `x : any` | rejected -- the union guide's own headline example does not compile | TUR-E0006 |
| G5 | `(f x)` where `f : any` | rejected | `'f' is not a function or continuation` |
| G6 | `(match x)` where `x : any` | rejected at elaboration, though `emit_expr.c:14356` already has a `TY_ANY` scrutinee arm | `match: scrutinee must be an ADT type, got any` |
| G7 | heterogeneous containers | element type unifies; `(vec-of 1 "two")` is rejected | TUR-E0001 |
| ~~G8~~ | `type-of` on a boxed closure | **FIXED 2026-09-07** -- both back ends answer `"fn"`. But the tag is the bare `TY_FN` kind, so it identifies functions as a CLASS and not by signature; see P9 | -- |
| G9 | `=`/`<`/`>` on `any` | rejected | TUR-E0006 |
| G10 | `any` truthiness in `if` | untested; `if` wants `bool` | -- |
| G11 | field access on `any` | no dynamic path | -- |

G3, G4, G5, G6, G9 are one problem wearing five hats: **the builtin operator
table is keyed on concrete argument kinds and has no `any` row.** That is the
core engineering work of this plan, and it is the same work on both back ends
-- except that on the interpreter the *implementation* already exists
(Section 2.3) and only the elaborator needs to stop rejecting the call.

G2 and G8 are defects independent of Saffron and were filed separately. **G2 is
fixed** ([inferred-return-defaults-inconsistently](../archive/inferred-return-defaults-inconsistently.md)
-- an ordering bug, not the missing inference the report guessed); G8 is
`docs/reported/type-of-on-boxed-closure-diverges.md`, and G4's documentation
half is `docs/reported/any-type-guide-examples-do-not-compile.md`.

Two further defects were found by the follow-up research into S0 and D8, and
both are prerequisites rather than side notes -- `any-type-ids-are-per-tu.md`
(the box tag means different things in different translation units) and
`forall-dict-byvalue-receiver-emits-uncompilable-c.md`. Neither is in the gap
table above because neither is a *missing* capability; they are existing
machinery that is wrong. See S0's P1-P5 table.

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
this already exists -- it is what makes an unannotated `defn` return infer at
all (G2), and fixing G2 confirmed the inference block is already in `elab_defn`
rather than something Saffron would have to add.

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
method call. Runtime instance dispatch stays unscheduled -- but it is a
smaller, better-founded piece of work than the first draft of this plan
assumed, and the shape below is what it would be.**

The first draft asserted this was tractable "because `emit_any_type_id`'s name
table is already a runtime type registry". That was a guess. It was then
measured, and the picture is more favourable in one direction and more
constrained in another.

#### What was measured

**(a) The static path monomorphizes; there is nothing to reuse.** A
constrained `defn` produces one specialization per instantiating type:

```c
static double describe__spec__double_tur_adt_Circle(tur_adt_Circle x);
static double describe__spec__double_tur_adt_Square(tur_adt_Square x);
```

Under Saffron the argument is `any` and pins no type, so there is nothing to
specialize on. Runtime dispatch is not an optimisation of the static path --
it is a different path.

**(b) Per-instance dict types are not the obstacle they look like.** Each
instance gets its own C struct with concrete-typed method pointers, and there
is no `dict_Shape` supertype:

```c
typedef struct dict_Shape_Circle { double (*area)(tur_adt_Circle); } dict_Shape_Circle;
typedef struct dict_Shape_Square { double (*area)(tur_adt_Square); } dict_Shape_Square;
```

But the existing dispatch site already punches straight through that, casting
the dictionary to `void **` and indexing by slot:

```c
/* the mode-B dict-clone body, from tests/fixtures/forall-dict-show */
static int64_t poly_hyshow_un_undict_un1444(int64_t __dict_1445, int64_t x) {
    const char *__ps_40 =
        (((const char * (*)(int64_t))((void **)(intptr_t)__dict_1445)[0])(x));
```

So **a dictionary is already an `int64_t` at runtime and a method is already
reached as `((void **)dict)[slot]`.** The plumbing D8 needs largely exists and
ships; `forall-dict-pass` graduated 2026-07-06.

**(c) What is actually missing is instance *selection*.** In mode B the
**caller** picks the singleton, from a type it knows statically:

```c
((int64_t(*)(void*, int64_t, int64_t))f.fn)(f.env,
    (int64_t)(intptr_t)(&dict_Show_int_singleton), (int64_t)(INT64_C(7)));
```

Saffron has no such caller. The selection has to come from the value's box tag:
`dict = registry[class][tag]`. That registry is exactly what S0's P1 fix
builds -- so **P1 is a hard prerequisite for D8**, and conversely D8 is
cheaper than it looks *given* P1.

**(d) The carrier pun does not survive by-value receivers.** The cast above is
honest only because `int` and `bool` both ride the int64 carrier. With a
by-value struct receiver the same machinery emits uncompilable C:

```
error: incompatible type for argument 1 of
  '(double (*)(tur_adt_Square))*(void **)__dict_1449'
  note: expected 'tur_adt_Square' but argument is of type 'int64_t'
```

That is a defect in its own right -- `forall-dict-pass` guards its other
unsupported shape with TUR-E0311, and this one has no guard -- and it is filed
as
[forall-dict-byvalue-receiver-emits-uncompilable-c](../reported/forall-dict-byvalue-receiver-emits-uncompilable-c.md).
For D8 it is the load-bearing constraint: dictionary slots must hold
**per-instance carrier wrappers**, not raw instance functions:

```c
static double __dictwrap_Shape_area_Circle(int64_t c) {
    return __inst_Shape_area_Circle(*(tur_adt_Circle *)(intptr_t)c);
}
```

Then every slot has one uniform carrier signature and the pun becomes honest.
The box/no-box decision is the same `emit_type_is_byvalue_adt` predicate `any`
widening already uses, so the two paths should share it.

**(e) The interpreter splits from the compiled path here too.** `TypeClassEnv`
(`typeclass.c:166`) is a compile-time linked list searched by `TypeKind` plus
`AdtDef` pointer, living in the compiler arena. Under `--interpret` that env is
*live at eval time*, and values already carry their type name
(`turi_any_named_type`). So interpreted runtime dispatch is a lookup against
machinery that is already in memory; compiled runtime dispatch needs the table
emitted. **This is the same asymmetry as Section 2.3**, which is a good sign
the staging model generalises rather than being special-pleading for S3/S4.

#### How big is the limitation, actually? (measured)

Smaller than "no typeclasses in Saffron" sounds, because **the operators a
dynamic program leans on are not typeclass methods.** `+ - * / = < > <=`
and `println` are rows in the builtin operator table
(`builtins.c:28` -- `{ "=", NULL, 2, 2, {.kind=TY_INT}, {.kind=TY_BOOL},
BS_BIN_INFIX, "==" }`), not `Eq`'s `eq?`. They are D4's problem, and D4 solves
them for `any` without any instance resolution at all. Ordinary Saffron code --
arithmetic, comparison, printing, string work, control flow -- never reaches
D8's limit.

What does reach it: user-defined classes, the HKT stack (`fmap`, `bind`,
`pure`), `Eq`'s `eq?`, and `Hash`/`MapKey`, which `Map` requires of its keys.

**The escape hatches work for a MONOMORPHIC receiver:**

| Route | Status | Shape |
|---|---|---|
| `is?`-guard narrowing | **works today** | `(if (is? x Circle) (area x) ...)` -- verified: 19.6349 / 50.41 |
| explicit `cast` | **works today** | `(area (cast x Circle))` |
| annotate the binding | works trivially | `[x : Circle]` |
| `@TypeName` witness | **works (fixed 2026-09-07)** | `(tag-of @bool x)` -- pins the instance and unboxes, checked |

The first is the important one, and it is not ceremony: it reads as a
type-case, which is how dynamic languages dispatch anyway (Clojure's
`condp instance?`, Racket's predicate `cond`). A Saffron program that wants
per-type behaviour writes the type-case it would have written regardless.

The fourth row used to read "broken on `any`", which mattered more than a
missing convenience: `@TypeName` is what the compiler's own ambiguity
diagnostic tells you to reach for, so following the hint produced a `cc`
error. It now pins the instance **and** unboxes the receiver, checked -- one
token, and a wrong witness panics rather than reinterpreting the payload. The
same change made a method call on an un-narrowed `any` a diagnostic naming all
three routes, where with a single instance in scope there had been no
diagnostic at all. See
[typeclass-dispatch-on-any-receiver-emits-uncompilable-c](../archive/typeclass-dispatch-on-any-receiver-emits-uncompilable-c.md);
its single-instance half had a worse cause than filed, recorded there.

**For a PARAMETRIC or HKT receiver, none of them work.** This was measured
after the fact and it inverts the paragraph above for the entire `Functor` /
`Applicative` / `Monad` stack, plus `Option` and `Result`:

| Attempt on an `any` holding `(Some 7.1)` | Result |
|---|---|
| `(is? x Option)` | **silently `false`** |
| `(cast x Option)` | **panics**: `cast: any holds Option, not Option` |
| `(is? x (Option float))` | `error: 'is?' expects a type name as second argument` |
| `(cast x (Option float))` | same |

with `type-of x` answering `"Option"` the whole time, so nothing in the
language surface indicates what is wrong. The cause is that
`emit_any_type_id` interns by `type_name`, which renders a `TY_APP` **per
instantiation** -- deliberately, so `(Box int)` and `(Box float)` stay
distinct -- while a bare `Option` target resolves to the head type, a
different key. Two ids get interned and both are `shown` as `"Option"`:

```c
case 1000: return "Option";      /* the widen: (Option float) */
case 1001: return "Option";      /* the is? target: bare Option */
```

**FIXED 2026-09-07** --
[any-narrowing-broken-for-parametric-receivers](../archive/any-narrowing-broken-for-parametric-receivers.md).
`is?` and `cast` now share one target resolver: an applied target
`(Option float)` interns the same `TY_APP` the widen site did, and a bare
constructor is a hard error naming the arity instead of a silent `false`. So
the type-case idiom reaches the HKT stack, on both back ends:

```turmeric
(defn dyn-double [x : any] : float
  (if (is? x (Option float))
    (unwrap-or (fmap (cast x (Option float))
                     (fn [v : float] : float (* v 2.0)))
               0.0)
    0.0))                                    ;; => 14.2, compiled and interpreted
```

`tests/fixtures/any-narrow-parametric-roundtrip`. With that, the parametric
case rejoins the monomorphic one and the "how big is the limitation" answer
above holds uniformly -- which was the point of fixing it before scheduling
anything else here.

Two limits survive, both recorded rather than silently absorbed. The
`if`-guard narrowing does **not** extend to an applied target, so the `cast`
above is load-bearing rather than decorative -- narrowing recognises the
simple `(is? x T)` shapes the union guide documents, and widening it is its
own change. And the interpreter cannot discriminate two instantiations of one
constructor (a `TuriValue` carries the ADT, not the type argument), so it
head-matches; `any-narrow-parametric-discriminates` and
`any-cast-wrong-instantiation` are compiled-only for exactly that reason.

A second, independent gap sits next to it: a *generic defn* called in an
`: any` return position is never monomorphised, so
`(defn f [] : any (some 7.1))` emits a call to an undeclared `some` and fails
at `cc` (writing `(Some 7.1)` directly works). Filed as
[generic-fn-in-any-return-position-emits-uncompilable-c](../reported/generic-fn-in-any-return-position-emits-uncompilable-c.md).

`@TypeName` (`elab_typeclasses.c:5563`) is the dedicated syntax for exactly
this situation -- and it is what the compiler's own ambiguity diagnostic
recommends -- but it pins the instance without unboxing the receiver, so
following the hint produces a `cc` error. Filed as
[typeclass-dispatch-on-any-receiver-emits-uncompilable-c](../reported/typeclass-dispatch-on-any-receiver-emits-uncompilable-c.md).
**Fixing it is the cheapest large improvement to D8's ergonomics available**:
the witness already names the target instance, which is precisely what `cast`
needs, so it can lower to dispatch-plus-checked-unbox and become a one-token
answer to the erased-receiver case.

**Where the hatches genuinely run out:** a heterogeneous collection crossed
with an open (user-extensible) class. `(map show xs)` over a vec holding three
user types cannot be narrowed at one site -- the dispatch has to happen
per element, so the user ends up hand-writing the type-case that a vtable
would be. That is the real cost, and S6 makes heterogeneous collections the
default, so it is not a corner case.

**Where they do not run out, and this bounds the damage:** a *closed* instance
set can have its type-case written once, in the prelude, instead of by every
caller. `MapKey` has five stdlib instances (`int`, `bool`, `cstr`, `float32`,
`float`) plus `String` and `Sym` -- a finite set the prelude controls. So
S6's `assoc`/`get` can dispatch dynamic keys with one `is?` chain and Saffron
gets working maps without D8. The same trick covers `Hash` and `Eq`.

So the limitation has two halves, and only the first is narrow:

- **Monomorphic receivers: narrow.** Closed classes are a prelude
  implementation detail; open classes over heterogeneous data are where a user
  feels it, and the type-case they write is idiomatic anyway.
- **Parametric / HKT receivers: total, until the narrowing bug is fixed.**
  No route reaches a method on an `any`-held `Option`. `fmap`/`bind`/`pure`,
  `Option`, `Result` and every monadic pipeline are on the far side of it.

That second half is a **prerequisite, not a D8 scheduling question** -- the
narrowing bug is wrong for shipping Turmeric regardless of Saffron, and once
it is fixed the parametric case rejoins the monomorphic one (a type-case over
`Option` / `Result` / user constructors, written once). Until then, any
estimate of "how much does D8 cost us" is measuring the bug, not the design.

#### The verdict, restated

D8 stays unscheduled, and Saffron ships with `(show x)` on an `any` an error
pointing at `cast`. But the reason is no longer "it is the single largest item
that could be attached" -- it is that D8 needs P1 and P2 fixed first, and the
open design questions are the ones the measurement did **not** answer:
superclass chains, default methods, HKT receivers, and what happens when two
instances match a tag. Those are worth deciding on evidence from real Saffron
programs, not up front.

Rough shape if it is ever built: one class-level dict type with carrier-shaped
slots; per-instance wrappers from (d); registration into P1's table at
static-init; a `registry[class][tag]` lookup at the call site; a clean
"no instance for T" panic. Four of those five pieces already have a home.

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

### S0 -- prerequisites (was "small"; the id fix is medium)

Five filed defects, all wrong regardless of Saffron. The first is the one that
matters: **it was an open question in the first draft of this plan and has
since been measured. The answer is that the ids are NOT stable, and Saffron
cannot be built until they are.**

| # | Report | Blocks |
|---|---|---|
| ~~P1~~ | ~~any-type-ids-are-per-tu~~ | **DONE 2026-09-07.** The id is a hash of the identity key, not the per-TU intern index, and the name table became a registry each TU publishes `{id, name, boxed}` rows into. Multi-TU builds now agree; the drop reads the minting TU's boxed flag. This also builds the runtime type registry D8 would key instance lookup off, as anticipated. [Archived](../archive/any-type-ids-are-per-tu.md) |
| ~~P2~~ | ~~forall-dict-byvalue-receiver-emits-uncompilable-c~~ | **DONE 2026-09-07** (guarded). The by-value shape is now a diagnostic, not uncompilable C. D8's carrier-wrapper work is smaller than the report implied -- the caller already boxes into the carrier, so only a per-instance deref wrapper in the dict slot is missing -- but it is a dictionary-ABI change and stays with D8. [Archived](../archive/forall-dict-byvalue-receiver-emits-uncompilable-c.md) |
| ~~P2b~~ | ~~typeclass-dispatch-on-any-receiver-emits-uncompilable-c~~ | **DONE 2026-09-07.** `@TypeName` on an `any` receiver now implies the checked unbox, so the witness is a complete one-token answer to an erased receiver; and a method call on an un-narrowed `any` is a diagnostic naming all three routes instead of uncompilable C. [Archived](../archive/typeclass-dispatch-on-any-receiver-emits-uncompilable-c.md) |
| ~~P2c~~ | ~~any-narrowing-broken-for-parametric-receivers~~ | **DONE 2026-09-07.** Was: `is?` on an `any`-held `Option` silently false, `cast` panicking `holds Option, not Option`. `is?`/`cast` now share one target resolver, take an applied `(Option float)`, and reject a bare constructor with a diagnostic. The type-case idiom reaches the HKT stack on both paths. [Archived](../archive/any-narrowing-broken-for-parametric-receivers.md) |
| ~~P2d~~ | ~~generic-fn-in-any-return-position-emits-uncompilable-c~~ | **DONE 2026-09-07.** Not a missing monomorph request -- elaboration was already correct, and `emit_abi_scan_expr` simply had no case for `EX_UNION_INJECT`, so a call under a widen was never scanned. Four cases added. Wider than filed: argument position and user generics too. [Archived](../archive/generic-fn-in-any-return-position-emits-uncompilable-c.md) |
| ~~P3~~ | ~~inferred-return-defaults-inconsistently~~ | **DONE 2026-09-07.** Was an ordering bug, not a missing inference: the conflict check ran before the block that adopts the body's type, so it compared against the un-inferred `TY_NIL`. Unannotated returns are now inferred for every body type, float included. [Archived](../archive/inferred-return-defaults-inconsistently.md) |
| ~~P4~~ | ~~type-of-on-boxed-closure-diverges~~ | **DONE 2026-09-07.** A `TY_FN` tag now answers "fn" on both back ends. [Archived](../archive/type-of-on-boxed-closure-diverges.md) |
| P9 | [any-fn-tag-does-not-discriminate-signatures](../reported/any-fn-tag-does-not-discriminate-signatures.md) | **high; D4/S4.** Found fixing P4. An `any`-boxed function matches every function type, so `cast` to a wrong signature miscalls silently. Its fix direction 2 (intern the fn type so the id is real) is also the prerequisite for CALLING an `any`-held function, which is D4's dynamic-call row |
| P5 | [any-type-guide-examples-do-not-compile](../reported/any-type-guide-examples-do-not-compile.md) | docs only |
| ~~P6~~ | ~~any-coercion-not-driven-by-expected-type~~ | **DONE 2026-09-07.** An `: any` annotation now widens at all four positions that take one -- `let`, `def`, the `if` join, and letrec/named-`let` accumulators -- each of which had a distinct cause. These are the positions S2/S6 lean on hardest. [Archived](../archive/any-coercion-not-driven-by-expected-type.md) |
| P7 | [interp-native-ctor-loses-adt-name](../reported/interp-native-ctor-loses-adt-name.md) | S3/S4 -- `type-of` diverges between the back ends for natively-constructed stdlib values, and the interpreter is the path Saffron ships on first |
| ~~P8~~ | ~~forall-dict-float-result-truncated~~ | **DONE 2026-09-07.** A float result through a mode-B dict clone was silently truncated (2.5 -> 2); both ends of the carrier crossing now bit-reinterpret instead of converting. Not Saffron-specific, but it was on the runtime-dictionary machinery D8 would build on. [Archived](../archive/forall-dict-float-result-truncated.md) |

#### P1 -- the id question, answered

`emit_any_type_id` interns into the per-TU `EmitCtx` and returns
`TUR_ANY_ID_BASE + first-seen index`, so **the same type gets a different id in
each translation unit.** Measured on `tur build --shared`, where `amod.c` tags
`Beta` as 1000 while `main.c` reads 1000 as `"Gamma"` and tests `is? v Beta`
as `== 1001`. Four wrong behaviours follow, all reproduced:

1. `type-of` returns another type's name.
2. `is?` is a false negative on the correct type.
3. A valid `cast` panics -- `cast: any holds ByVal, not HeapThing` on a value
   that genuinely is a `HeapThing`, with both names wrong.
4. `__tur_any_drop` consults the wrong `boxed` flag, so a TU calls `free()` on
   a handle another TU owns (or leaks, in the mirror case).

The first draft's probe passed only because **`tur build <dir>` folds the whole
project into one TU** -- confirmed by wrapping `CC`: one `.c`, both modules
inside, one consistent table. `--shared` and `emit-c --output-dir` (the CMake
path) both split, and both diverge.

This reclassified S0 from bookkeeping to a real prerequisite, because Saffron
makes `any` the type of nearly every cross-module value.

**FIXED 2026-09-07** --
[any-type-ids-are-per-tu](../archive/any-type-ids-are-per-tu.md). The id is now
FNV-1a over `type_name`, forced clear of the `TypeKind` range, so it needs no
coordination between TUs; `__tur_any_name_ext` became a registry each TU
publishes `{id, name, boxed}` rows into at static-init, replacing the single
function pointer every TU overwrote. Mode 4 falls out for free: the `boxed`
flag rides the same row as the name, so a drop reads the flag the *minting* TU
published rather than its own table's. The original repro went from
`Gamma / Gamma / 0` to `Gamma / Beta / 1`.

Pinned by `tests/run-any-type-id-multi-module.sh` (ctest
`tur_any_type_id_multi_module`), which drives separate compilation because
`tur build <dir>` inlines everything into one TU and hides the bug -- and which
was verified to fail without the fix, with all four behaviours visible. One
cost recorded: `__tur_any_find` is a linear walk, and `__tur_any_drop` calls it
at every scope exit owning an `any`; if that ever measures, the answer is an
index built at startup, not a return to per-TU numbering.

The registry was not throwaway scaffolding, as anticipated: it is the runtime
type registry D8 would key instance lookup off, so this is the first half of
D8's foundation whether or not D8 is ever scheduled.

**Exit:** ~~P1~~ done, with the multi-TU pin the report asked for; P2-P5 fixed
or archived (P2c done, see D8 above).

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

1. ~~**Cross-TU `any` id stability.**~~ **ANSWERED, and the answer was no.**
   The ids diverge per translation unit and four behaviours are wrong on
   `tur build --shared` today. Moved from an assumption to a hard S0
   prerequisite (P1); see
   [any-type-ids-are-per-tu](../reported/any-type-ids-are-per-tu.md). Kept
   here rather than deleted because the first draft's probe said "correct" and
   the reason it did -- `tur build <dir>` is single-TU -- is the kind of thing
   that would otherwise get re-derived from scratch.
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

### A.3 -- cross-TU `any` ids (ANSWERED: they diverge)

Two-module project, `amod` widening a `Beta` to `any`, `main-mod` interning
`Gamma` *first* so its local id ordering differs from `amod`'s. The same
program, two build modes:

```
$ tur build . && ./build/bin/anyids            # single-TU
Gamma
Beta
1                                              # correct

$ tur build --shared .                         # multi-TU (link obj/*.c, run)
Gamma
Gamma                                          # WRONG: a Beta reports as Gamma
0                                              # WRONG: (is? v Beta) is false
```

The emitted C, side by side:

```c
/* obj/amod.c */  case 1000: return "Beta";     TUR_TAG(1000, ...)   /* Beta */
/* obj/main.c */  case 1000: return "Gamma";
                  case 1001: return "Beta";     TUR_GETTAG(v) == 1001  /* is? */
```

A third mode surfaces when `main` casts the value it received -- the cast is
valid and panics anyway, naming the wrong type in both positions:

```
panic at tur_runtime.h:1817: cast: any holds ByVal, not HeapThing
```

and a fourth in `__tur_any_drop`, whose per-TU `boxed` flag makes one TU
`free()` a handle another TU owns.

**Why the first draft's probe said "correct":** `tur build <dir>` folds the
whole project into ONE TU. Confirmed by wrapping `CC` and capturing its
inputs -- one `.c` (7445 lines, both modules inside), one consistent table.
`--shared` passes three, and `emit-c --output-dir` (the CMake-consumer path)
splits the same way.

Full write-up:
[any-type-ids-are-per-tu](../reported/any-type-ids-are-per-tu.md).

### A.5 -- runtime typeclass dictionaries (D8)

A constrained `defn` monomorphizes -- no dictionary survives:

```c
static double describe__spec__double_tur_adt_Circle(tur_adt_Circle x);
static double describe__spec__double_tur_adt_Square(tur_adt_Square x);
```

The mode-B rank-2 path *does* carry a dictionary at runtime, as an `int64_t`
reached by slot index (`tests/fixtures/forall-dict-show`):

```c
static int64_t poly_hyshow_un_undict_un1444(int64_t __dict_1445, int64_t x) {
    const char *__ps_40 =
        (((const char * (*)(int64_t))((void **)(intptr_t)__dict_1445)[0])(x));
```

with the caller choosing the singleton from a statically known type:

```c
((int64_t(*)(void*, int64_t, int64_t))f.fn)(f.env,
    (int64_t)(intptr_t)(&dict_Show_int_singleton), (int64_t)(INT64_C(7)));
```

The same shape with a by-value struct receiver does not compile:

```
$ tur run tc3.tur
error: incompatible type for argument 1 of
  '(double (*)(tur_adt_Square))*(void **)__dict_1449'
  note: expected 'tur_adt_Square' but argument is of type 'int64_t'
tur: cc invocation failed (status 256)
```

Filed as
[forall-dict-byvalue-receiver-emits-uncompilable-c](../reported/forall-dict-byvalue-receiver-emits-uncompilable-c.md).

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
