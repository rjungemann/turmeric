# R7RS-small as a `#lang` over the Turmeric runtime

Status: **R0 and R1 landed 2026-09-23.** `#lang r7rs` is a base
(`LANG_R7RS` + `READER_R7RS`, ninth row of `LANG_BASES[]`), the `r7rs`
`EXPERIMENTS[]` row gates it with the directive as its own enable, and the
Scheme reader variant reads every lexeme R1 lists. There are no Scheme
SEMANTICS yet: a `#lang r7rs` file elaborates exactly as the same forms would
under `#lang saffron`, which is R1's exit criterion and is pinned by
`tests/fixtures/r7rs-elaborates-as-saffron`. R2 onward is unbuilt. Each landed
stage carries a "What shipped" note below.

Every "today" claim in Sections 2 and 3 was **measured on 2026-09-21** against
`./build/tur` at v0.50.0, Debug build, and the transcript is in
[Appendix A](#appendix-a----probe-transcript). That is deliberate: the Saffron
plan records that **six of its eight defect reports had a diagnosis that was
wrong on inspection**, always because control flow was read and a cause
inferred rather than measured. Treat any claim here that does *not* carry a
probe as a lead to verify, not a finding.

---

## 0. The ask, and the honest size of it

> In a similar vein to how Saffron grew as a `#lang`, I want R7RS-small Scheme
> as a `#lang` as well. This will require more work than Saffron. But if we can
> support Turmeric libraries that is what I have in mind.

Both halves of that are right, and the second half is the one that should drive
the design.

**"More work than Saffron" is correct**, but not for the reason it looks like.
Saffron's expensive part -- making `any` an *operational* type rather than a
storage type, on both back ends -- is done, and R7RS inherits all of it.
R7RS's expensive part is somewhere else entirely: it is a **specification with
a conformance test suite**, and the last 10% of a spec is most of the work.
Four items in Section 3 are hard requirements of R7RS that Turmeric does not
meet today, and the first of them -- proper tail calls on the compiled path --
turned out on measurement to be *not met at all* for the shape Scheme is made
of. It is large enough to have its own document
([proper-tail-calls-plan.md](proper-tail-calls-plan.md)) and is a prerequisite
rather than a stage.

**"Support Turmeric libraries" is the whole point**, and it is also the thing
that makes this worth doing rather than an exercise. A Scheme that can
`(import (turmeric stdlib/hamt))` and get a persistent HAMT, or
`(import (turmeric stdlib/session))` and get session-typed channels, is a
Scheme nobody else ships. A Scheme that reimplements `(scheme base)` on its own
runtime is one of forty. So the seam is scheduled **early** (R3), not at the
end, and the plan is arranged so that if the seam does not work, we find out
before building a library.

---

## 1. Thesis

**R7RS-small is not a third language. It is Saffron with a Scheme reader, a
Scheme prelude, and four pieces of runtime that Saffron did not need.**

The load-bearing measurement behind that sentence is in
[Section 2.1](#21-the-dynamic-substrate-is-built-and-it-is-not-saffron-specific):
the 71 places in the compiler that test for Saffron are, with essentially no
exceptions, asking **"is this file dynamically typed?"** -- not "is this file
Saffron?". The canonical one is a single line:

```c
/* src/compiler/elab_fns.c:5649 */
return lang_span_is_saffron(sp) ? TY_ANY : TY_INT;
```

Rename that predicate to `lang_span_is_dynamic`, back it with a trait table
instead of an enum compare, and `#lang r7rs` inherits:

- unannotated parameters and returns defaulting to `any`;
- dynamic arithmetic, comparison, truthiness, print, call, field access, and
  `match` on `any`, **on both back ends**;
- the `any` runtime type-id machinery, which interns each named type's
  monomorph and hands out a distinct runtime tag -- this is what will give
  Scheme its type predicates (`char?`, `pair?`, `port?`) for free;
- the checked cast at the boundary to typed Turmeric code, in both directions,
  already pinned by `tests/run-saffron-import.sh`.

That inheritance is not a nice-to-have. It is what separates "a big feature"
from "a fork of the elaborator", and it is the only thing that makes this
affordable. **If Saffron and R7RS end up as two dynamic substrates, the plan
has failed**, and that failure will be visible as the first
`if (lang == LANG_R7RS)` that sits beside an existing `LANG_SAFFRON` test
rather than replacing it.

The genuinely new work is five things, and they are enumerated so the list can
be argued with:

1. **A Scheme reader.** A variant of the existing reader, not a second reader.
2. **A real datum.** `quote` must construct runtime data. It does not today.
3. **`syntax-rules`.** Turmeric's macros are deliberately unhygienic.
4. **Proper tail calls and re-entrant `call/cc`** on the compiled path.
5. **`(scheme base)` and its eleven siblings**, written as adaptors over the
   typed stdlib wherever one exists.

---

## 2. What already ships (measured)

### 2.1 The dynamic substrate is built, and it is not Saffron-specific

```
$ grep -rn "LANG_SAFFRON\|lang_span_is_saffron\|g_opt_saffron" src/ \
    --include=*.c --include=*.h | grep -v generated | wc -l
71
```

across 19 files. Reading them, the tests fall into three groups:

| Group | Count (approx.) | What it is actually asking |
|---|---|---|
| Elaboration defaults and the dynamic surface (`elab_fns.c`, `elab_call.c`, `elab_core.c`, `elab_forms.c`, `elab_structs.c`, `elab_effects.c`, `elab_typeclasses.c`, `elab_toplevel.c`) | ~45 | "is this file dynamically typed?" |
| The emitter gate (`emit_module.c`, `globals.c`) | ~6 | "does this build need the `any` type and instance registries?" |
| Genuinely Saffron-named surfaces (`main.c` `--lang saffron`, `tur init --saffron`, the prelude hook, `lang_dialects.c`) | ~20 | "is this file Saffron specifically?" |

The first two groups -- roughly three quarters -- want a **trait**, not an
identity. Converting them is mechanical, reviewable, and has no behavior change
on its own, which is exactly what makes it a good R0.

### 2.2 The reader already has three of Scheme's lexical oddities

Probed and working today, in an ordinary `.tur` file with no directive:

| Syntax | Status |
|---|---|
| `#\| block comment \|#` | **works** |
| `#;(datum comment)` | **works** |
| `'x` (quote abbreviation) | **works** -- `read_quote`, `reader.c:3520` |
| `,` between forms | **treated as whitespace** (`reader.c:143`, `:719`, `:726`) |

That last row is the good news disguised as a problem. `,` being whitespace is
a Clojure inheritance, and it means the byte is **not spoken for by any
Turmeric syntax** -- so giving `,` and `,@` their Scheme meanings under
`#lang r7rs` costs nothing and breaks nothing.

`#t` is a read error today (`unexpected character '#' (0x23)`), which is the
expected shape: `#`-dispatch is a closed set plus the reader-macro registry.

### 2.3 Symbols are a real runtime type

`TY_SYM` is an interned, process-lifetime, pointer-identity symbol
(`stdlib/sym.tur`), and `(quote foo)` already evaluates to one:

```
$ tur run q.tur
q.tur:3:5: error [TUR-E0006]: operator lookup failed for 'println':
           got 1 arg(s), first arg type Sym
```

The error is `println` lacking a `Sym` overload, which is a stdlib gap, not a
representation gap. Scheme's symbol type exists, with the right identity
semantics for `eq?`, and `sym-dynamic.tur` already has opt-in runtime
`string->symbol` interning -- which is exactly what `string->symbol` needs.

### 2.4 The interpreter already has general proper tail calls

Mutual recursion at depth 1,000,000, under `--interpret`: **completes**.
`eval_apply_inner` carries a TCO trampoline loop and `eval_body_tco` is a
tail-position dispatcher, so this is by design, not by luck.

This matters for staging. R7RS **requires** proper tail calls in all tail
positions (R7RS-small section 3.5). The interpreter meets that today; the
compiled path does not (see 3.1). So R7RS gets the same staging shape Saffron
used and for the same reason: **interpreter first, compiled second.**

### 2.5 Procedural macros already run the whole language at expansion time

`docs/archive/macro-system-direction-plan.md` is archived as executed. It froze
the bespoke compile-time evaluator and added `defmacro*`, whose body is
**ordinary Turmeric evaluated at expansion time by turi**, with a `Syntax`
value (`TY_SYNTAX`, wrapping a reader `Form*`) crossing the boundary.

That is the substrate `syntax-rules` wants. A `syntax-rules` transformer is a
pattern matcher plus a template instantiator over syntax objects -- which is to
say, an ordinary program over `Syntax`. It does not need a phase tower, and the
plan that decided against a phase tower gave reasons that still hold.

`gensym` freshness is also already stronger than a counter: a candidate is
checked against the symbol table, including every symbol the reader has seen in
the current file. That is the floor a renaming-based hygiene scheme stands on.

### 2.6 The Turmeric seam already works, in both directions

This is the user's actual ask, and it is the part that is *most* done.

- **Dynamic calling typed** -- Saffron D5(c): an implicit checked `cast` at each
  argument whose static type is `any` and whose parameter type is concrete. One
  tag compare; panics with `cast: any holds cstr, not int` on mismatch.
  `tests/fixtures/saffron-boundary-check` and `saffron-boundary-panic`.
- **Typed calling dynamic** -- a dynamic module's exports are `any`-typed and
  the Turmeric caller narrows. This was **broken and is fixed**: the import path
  in `elab_module.c` hardcoded `READER_TURMERIC` and never ran `#lang`
  detection, so a `#lang saffron` module could not be imported at all. Pinned by
  `tests/run-saffron-import.sh` on both back ends.

The second bullet is worth dwelling on, because it is the trap this plan is
most likely to hit again. **Two entry points have now been caught hardcoding
`READER_TURMERIC`** (`tur fmt`, and module import), each found only when
someone tripped over it. A third dialect will find the third one. R1 should
therefore include a deliberate sweep -- `grep -rn "READER_TURMERIC" src/` and
justify each site -- rather than waiting to be surprised.

---

## 3. What does not ship (measured)

### 3.1 The compiled path does not have proper tail calls

**This is the largest gap, and it is worse than the documentation suggests.**
It now has its own plan: [proper-tail-calls-plan.md](proper-tail-calls-plan.md),
which carries the full measurement matrix and the design. The summary:

| Shape | `-O0` | `-O2` | `--interpret` |
|---|---|---|---|
| self tail call | pass | pass | pass |
| self tail call in a `match` arm | no backedge emitted | no backedge emitted | pass |
| mutual (2 or 8 functions) | **SIGSEGV** | pass | pass |
| **indirect, through a `fn` value** | SIGSEGV | **SIGSEGV at ~29,335** | pass |

Three things follow, and the third is the one that matters here:

1. **The self-tail-call guarantee is real** -- a `__tur_tailcall:` label and a
   `goto`, surviving `-O0`. Not an optimizer favor.
2. **Mutual tail calls pass at `-O2` only because LLVM inlines small cycles into
   a loop.** Not sibling-call optimization, and not a language property: it
   evaporates at `-O0` and whenever the bodies exceed the inliner's threshold.
3. **The indirect case -- a call through a function value -- fails at `-O2` too**,
   at ~29K deep. That is the shape Scheme is made of: every Scheme procedure call
   is a call to a value. So the compiled path does not merely lack a guarantee
   here; it lacks the behavior.

The root cause is structural and is documented in-tree at `emit_fns.c:4089`:
`panic` is not `noreturn`, so **every** call site is followed by
`if (tur_panicking) return ...;`. No call in emitted Turmeric C is ever in C
tail position, which forecloses both sibling calls and `musttail` until that
changes.

### 3.2 `quote` is not a data constructor

```
$ tur run q2.tur           # (quote (1 2 3))
q2.tur:2:19: error: expression in call head has type `int`, which is not callable
```

`quote` yields a `Sym` for a symbol (2.3) and otherwise does not build runtime
structure at all: the list is elaborated as a **call**. And the dot is not
reader syntax:

```
$ tur check dotted.tur     # (quote (a . b))
dotted.tur:2:22: error: unbound symbol '.'
```

Scheme's `quote` over arbitrary structure -- proper lists, improper lists,
vectors, nested data -- is genuinely new machinery. See D4.

### 3.3 There is no char type, no mutable pair, no port

- **No `TY_CHAR`.** `#\a` has no type to be. `char?` must be distinguishable
  from `integer?`, so a `char` cannot simply be an int.
- **No mutable pair.** `stdlib/list.tur`'s `Cons` is a struct; `set-car!` and
  `set-cdr!` have no target. (`set-field!` exists, so mutation is *possible*;
  the pair type is what is missing.)
- **No ports.** `stdlib/io.tur` and `fs.tur` have file and stream operations,
  but not R7RS's port object with its textual/binary and input/output taxonomy,
  and not string ports.
- No `EofObject`, `Promise`, `Parameter`, `ErrorObject`, `Environment`.

None of these needs a new `TypeKind`. See D3.

### 3.4 `call/cc` is an escape continuation

`call/cc` ships and is unconditionally on, but
`docs/guides/delimited-control-operators-guide.md` is explicit: it is
**one-shot upward**. `k` defaults to `^unique`; invoking it after the prompt
has returned prints `tur: continuation invoked after its call/cc prompt
returned`. The implementation is a `setjmp`/`longjmp` landing.

`call/cc*` is the multi-shot, cloneable variant, and it is real -- but it
captures against a `cloneable-reset`, so it is **delimited**, and
`tests/fixtures/call-cc-star` drives it through raw runtime names
(`tur_cloneable_cont_resume`, `tur_cloneable_cont_clone`) rather than a
Scheme-shaped surface.

R7RS `call/cc` is unlimited-extent and multiply-invocable, including re-entry
after the capturing call has returned. That is the gap. See D7.

### 3.5 Exact integer arithmetic wraps silently

Exact integers are `int64_t` and overflow wraps. R7RS requires exact arithmetic
to be **exact** -- an implementation that cannot represent a result must signal
an error, not produce a different number. Silent wraparound is a wrong-answer
bug wearing a performance costume.

`Rational` exists (`stdlib/rational.tur`) but is `int64`/`int64`, so it inherits
the same problem. `Complex` exists over `double`. There are no bignums. See D8.

### 3.6 There is no hygienic macro system

`defmacro` is unhygienic **by design** (the macros guide says so in those
words), with `gensym` as the discipline. R7RS requires `define-syntax` /
`let-syntax` / `letrec-syntax` with `syntax-rules`, and hygiene is not optional
there -- the standard's own definition of `or` and `do` depends on it. See D5.

### 3.7 The `#lang` base set is rendered as a cross-product, and R7RS breaks it

`lang_dialects.c` does not tabulate the legal bases. It **computes** them:

```c
static const LangDialect DIALECTS[] = { LANG_TURMERIC, LANG_SAFFRON };
static const ReaderType  READERS[]  = { READER_TURMERIC, READER_CURLY_INFIX,
                                        READER_NEOTERIC, READER_SWEET };
```

and `lang_bases_count()` returns their product. `tur dialects` prints 8 rows.
The file argues for this explicitly -- "the legal bases are exactly their
cross-product, and a table would have to be kept in step with both."

That argument stops being true the moment a language arrives with its own
reader. `r7rs/sweet` is not a thing, and `turmeric/r7rs` is not a thing either.
The cross-product must become a table again. This is a small change, but it
must be made **deliberately**, because the current source will actively argue
against it during review.

The consolation is that the same file has already built the hook for this. The
`LangBaseDescriptor.experiment` field exists, is `NULL` on every row today, and
carries this comment:

> a future gated dialect fills it in here and is badged rather than hidden,
> with no consumer change.

R7RS is that dialect. The field has been waiting for it.

---

## 4. Design decisions

### D1 -- `LANG_R7RS` joins the language axis; the base set becomes a table

**Verdict: a third `LangDialect`, a fifth `ReaderType`, and a real
`LANG_BASES[]` table with one row per legal pair.**

```c
typedef enum LangDialect {
    LANG_TURMERIC = 0,
    LANG_SAFFRON,
    LANG_R7RS,
} LangDialect;
```

with a trait row per language, which is what removes the cross-product and
what R0's rename binds against:

```c
typedef struct LangTraits {
    const char *name;             /* "turmeric" | "saffron" | "r7rs" */
    ReaderType  default_reader;   /* READER_TURMERIC | READER_R7RS */
    bool        reader_axis_free; /* may be spelled over the four readers */
    bool        dynamic;          /* unannotated means `any` */
    const char *prelude;          /* autoload tail, or NULL */
} LangTraits;
```

`turmeric` and `saffron` keep `reader_axis_free = true` and contribute four
bases each; `r7rs` contributes exactly one. `tur dialects` goes from 8 rows to
9, and `lang_dialects_print` / `_print_json` / `lang_base_at` iterate the table
instead of multiplying two arrays. **Every current consumer keeps its shape** --
that is the point of the existing `LangBaseDescriptor`.

Spelling: **`r7rs`**, matching Racket. Not `scheme`: that name is version-free,
and `#lang scheme` already means something else to anyone arriving from Racket.
A future `r6rs` or `r7rs-large` is a sibling base token, never a slash suffix --
the slash axis is the reader, and R7RS does not have a spare reader axis to
give away.

### D2 -- the value representation is Saffron's `any`, and there is no second one

**Verdict: a Scheme value IS a `tur_tagged_t`. Do not introduce a
`SchemeValue`.**

This is the decision the rest of the plan hangs off, and the alternative is
tempting enough to be worth refuting. A dedicated Scheme value with NaN-boxing
or a pointer-tagged representation would be faster and more idiomatic for a
Scheme. It would also:

- fork the runtime, giving two dynamic value representations to keep in step;
- fork the interpreter, which already has exactly one (`TuriValue`);
- and, fatally, **destroy the seam** -- the entire Turmeric-interop story in
  2.6 is built on `any`, and a Scheme value that is not an `any` cannot cross
  it without a conversion layer at every call.

So: `any`. The cost is honest and should be stated rather than discovered:
Saffron's measured cost is one tag compare per boundary argument and a 16-byte
box for by-value aggregates, with no generational GC and no NaN-boxing
underneath. A Scheme on this substrate will allocate harder than a Scheme
designed for one. D3's rung ladder is where that gets addressed, not here.

**One concrete consequence**, found by reading `lang_dialect_apply`:
`g_opt_saffron` is what makes `emit_module.c` emit the `any` type and instance
registries and the dynamic-dispatch panic. It is set when a `#lang saffron`
file is *read*. `LANG_R7RS` must set the same flag, which is the R0 rename's
second job -- and the flag should lose its Saffron name at the same time.

### D3 -- Scheme's disjoint types are `defopaque`/`defstruct`, not new `TypeKind`s

**Verdict: no new `TypeKind` for char, pair, port, promise, parameter,
eof-object, error-object, or environment.**

Section 2.1 noted that `emit_any_type_id` interns each named type's monomorph
`type_name` and hands out `TUR_ANY_ID_BASE + index`, so `Point` and `Other` are
distinguishable at runtime and `type-of` reports the source-level name. That is
precisely R7RS's requirement that the standard types be **disjoint**: exactly
one of `char?`, `pair?`, `string?` ... answers `#t` for any object.

So each Scheme type is an ordinary Turmeric type in `stdlib/r7rs/`, and its
predicate is `is?` against that type:

| Scheme type | Turmeric spelling | Predicate |
|---|---|---|
| char | `(defopaque Char :int)` -- a Unicode scalar value | `(is? x Char)` |
| pair | `(defstruct Pair [car : any cdr : any])`, mutable | `(is? x Pair)` |
| port | `(defopaque Port ...)` over the existing fd/stream layer | `(is? x Port)` |
| eof-object | a zero-field singleton | `(is? x EofObject)` |
| promise | `(defstruct Promise [...])` for `delay`/`force` | `(is? x Promise)` |
| parameter | a closure over a `dynvar` (see D10) | `(is? x Parameter)` |
| error object | `(defstruct ErrorObject [message : any irritants : any])` | `(is? x ErrorObject)` |

This is cheap, it is testable on day one, and it keeps `types.h` out of the
blast radius entirely. It also means **`char` costs one line**, which is the
kind of thing worth knowing before scoping.

### D4 -- `quote` constructs a static immutable datum; pairs are otherwise mutable

**Verdict: two representations of the same `Pair` type -- a static one for
literals, a heap one for `cons`.**

R7RS section 4.1.2: "It is an error to alter a constant (i.e. the value of a
literal expression) using a mutation procedure." So a quoted datum may live in
`.rodata`, and it should: `'(1 2 3)` in a loop must not allocate.

Lowering: a `quote` form's datum is walked at elaboration, interned into a
per-TU static table of `Pair`/`Char`/`Sym`/vector nodes, and the `quote`
expression becomes a reference to the table's root. `cons` allocates an
ordinary heap `Pair`. `set-car!` on a static one is an error; detecting that
cheaply (an address-range test, or a spare bit in the tag) is an open question
in Section 8 rather than a settled design.

Two consequences worth naming now:

- **`equal?` must terminate on cycles.** R7RS requires it. Mutable pairs make
  cycles constructible in three lines, so this is a real requirement and not a
  corner. The union-find approach from the standard's own reference is the
  expected answer.
- **Pairs are `any`-carrying, so the region rules apply.** Per CLAUDE.md's
  Region Store Hooks rule, `set-car!` and `set-cdr!` write a caller's word into
  memory that can outlive a `with-region` bracket, so **both need
  `TUR_REGION_NOTE`** and a case in `tests/fixtures/region-escape-via-store`.
  This is exactly the "new `-set!` primitive joins the list in the same change"
  clause; it is called out here so it is designed in rather than found later as
  a use-after-rewind.

### D5 -- `syntax-rules` is a procedural macro over `Syntax`, with renaming hygiene first

**Verdict: build `syntax-rules` on the existing `defmacro*` / `Syntax`
substrate (2.5). Ship renaming-based hygiene at R4. Do not build a phase
tower.**

The archived macro-system-direction-plan already answered the phase question
for this codebase and the reasons still hold: Turmeric elaborates whole-program
from source in one pass, macros are define-before-use, and the problems phases
solve do not arise. Nothing about R7RS changes that -- R7RS has no phase tower
either (that is R6RS).

Hygiene has three honest options:

- **(a) Renaming.** Every identifier a template introduces that is not a pattern
  variable is renamed to a fresh symbol bound to the macro-*definition*
  environment's binding. Gets the standard cases right -- `or`, `swap!`,
  `my-if`, the standard `do` -- and is what several small conformant Schemes
  ship. Referential transparency is partial: a template referring to a binding
  the use site shadows can still go wrong in constructed cases.
- **(b) Full scope-set hygiene.** Correct in all cases. Requires syntax objects
  to carry scope sets through elaboration, which is a change to the elaborator's
  identifier resolution, not an addition beside it.
- **(c) Explicit renaming** (`er-macro-transformer`) as the primitive, with
  `syntax-rules` written on top.

**Ship (a) at R4, expose (c) as the escape hatch, schedule (b) as a named
follow-up with a failing test that demonstrates the gap.** Rationale: (a) plus
(c) is enough to run the conformance suite's macro section, (b) touches the one
part of the elaborator this plan otherwise leaves alone, and a *named failing
test* is a much better record of a known gap than prose claiming there is not
one.

`gensym`'s symbol-table-checked freshness (2.5) is what makes (a) sound against
hand-written names, which is normally the first thing to break.

### D6 -- proper tail calls are a Turmeric prerequisite, not R7RS work

**Verdict: this is spun out into
[proper-tail-calls-plan.md](proper-tail-calls-plan.md). R7RS depends on its T6;
T1-T3 there are worth landing regardless.**

Investigating 3.1 changed the shape of this decision twice, so the conclusions
are recorded there rather than restated here. Two of them overturn what an
earlier draft of this section proposed:

- **`musttail` is not available as a first move.** Every emitted call site is
  followed by a panic check (3.1), so there is no tail position for it to apply
  to. It becomes an option only after that is addressed -- and the plan's T-D2
  shows the check is simply *redundant* at a genuine tail call and can be
  dropped there, which is far cheaper than restructuring `panic`.
- **"Route it through the CPS-IR backend" is disproven, not pending.** The
  indirect probe already routes through that backend and still overflows: the
  call is emitted as an ordinary C call plus a panic check, and each level
  re-enters through the direct-entry wrapper's `setjmp` and `dk_prompt` malloc.
  `CT_TAILCALL` exists in the IR vocabulary; this shape does not reach it.

What R7RS actually needs is **T6**: a bounce trampoline over the uniform
fat-closure representation, which D2's "a Scheme value is an `any`" decision is
what makes natural.

> **T6 landed 2026-09-23** for Saffron's dynamic calls -- the same fat-closure
> protocol with `any` arguments and result that `#lang r7rs` procedures will
> ride, so R6 inherits it rather than building it. A dynamic call in tail
> position runs 10,000,000 deep at `-O0`, and `^tailcall` accepts one. What
> R6 has to confirm is only that its calls lower to the same `EX_DYN_CALL`
> node; see the tail-calls plan's T-D6 "What shipped". The typed-Turmeric half of the problem (T-D3's limit: an
owned value live across a tail call is not a tail call) is nearly vacuous for
Scheme, which has no destructors.

The interpreter needs none of this -- it is correct on every row of 3.1's matrix
today -- which is why R2-R5 can be asserted under `--interpret` before the
compiled path has an answer.

### D7 -- `call/cc`: escape first, re-entrant second, and say so

**Verdict: stage it, and refuse to claim conformance until it closes.**

R6 ships `call/cc` at the level Turmeric already has -- one-shot upward escape,
plus `call/cc*`'s cloneable multi-shot behind a Scheme-shaped surface. That
covers the overwhelming majority of real Scheme code: early exit from loops,
`call-with-current-continuation` as a `return`, generators, backtracking.

Re-entry after the capturing call has returned gets a **named, documented
error**, not undefined behavior, and the escape hatch is `--interpret`, where
the explicit-stack evaluator is the natural substrate for reifying a
continuation (the archived turi trampoline plan says so in those words).

Full re-entrancy routes through D6(d) -- the CPS path -- because that is where
a heap-allocated continuation chain already exists. It is scheduled in R6 and
it is **the single largest item in the plan**. The plan should be read as: a
conformance *claim* is gated on this, and everything before it is a Scheme with
a documented deviation, which is a perfectly respectable thing to ship and ship
early.

### D8 -- exact integers signal on overflow; bignums are a separate epic

**Verdict: checked overflow at R5. Bignums named, scoped, and deferred --
plausibly to a spice.**

R7RS lets an implementation limit the range of exact integers; it does **not**
let one silently return the wrong number. So the minimum viable conformant
answer is a checked add/subtract/multiply on the exact path that raises a
Scheme error object on overflow. That is a small, well-understood change and it
converts a wrong-answer bug into a diagnosable one.

Bignums are a real library. GMP is LGPL, which is a licensing decision rather
than an engineering one, so the plausible shapes are a small in-tree bignum or
a `tur-bignum` spice that `(scheme base)` loads when present. `(scheme complex)`
and full exact rationals are **optional** in R7RS-small, so `Rational` and
`Complex` can be offered with their current `int64`/`double` limits documented
without endangering the conformance claim.

`(exact->inexact)`, `(inexact->exact)`, `exact-integer?`, and the
exactness-preservation rules are R5 work regardless of bignums.

### D9 -- `define-library` maps onto Turmeric modules; `(turmeric ...)` is the seam

**Verdict: library names are module paths, and the Turmeric namespace is one
head symbol.** This is the user's ask, and it is mostly a naming convention
over machinery that exists.

```scheme
#lang r7rs
(import (scheme base)
        (scheme write)
        (turmeric stdlib/hamt)                        ; a stdlib module
        (turmeric json/encode)                        ; a spice, resolved by build.tur
        (only (turmeric stdlib/vec) vec-new vec-push!))

(define m (hamt-new))
(display (hamt-get (hamt-set m "k" 42) "k"))
(newline)
```

The mapping:

| R7RS | Turmeric |
|---|---|
| `(scheme base)`, `(scheme write)`, ... | `stdlib/r7rs/base.tur`, `stdlib/r7rs/write.tur` |
| `(turmeric a/b/c)` | `(import a/b/c)` -- the tail joins with `/` |
| `(only (lib) a b)` | `(import lib :refer [a b])` |
| `(rename (lib) (a b))`, `(prefix (lib) p:)` | elaboration-level renaming over the same import |
| `(except (lib) a)` | import minus `a` |
| `(define-library (my utils) (export ...) (import ...) (begin ...))` | `(defmodule my/utils (export ...) ...)` |
| `(include "f.scm")` | the existing `(load ...)` path |
| `cond-expand` | a feature-identifier list, seeded from the build |

Argument passing across the seam is D5(c) from the Saffron plan, unchanged: an
implicit checked `cast` per argument whose parameter type is concrete. One tag
compare, and a panic naming both types on mismatch. **Nothing new is needed for
this; it is the thing that is already built and already pinned.**

The reverse direction -- a Turmeric file importing a `#lang r7rs` module and
narrowing its `any` exports -- likewise works via the S7 fix, and should get its
own `tests/run-r7rs-import.sh` on the model of `run-saffron-import.sh` rather
than being assumed from the Saffron one.

**Spices work by inheritance**: manifest-driven build descent already reads
`#lang` per file, so a `.scm` (or `#lang r7rs` `.tur`) file under a spice's
`src/` is an ordinary member. Worth a fixture, not a stage.

### D10 -- `parameterize` is `dynvar`, `guard`/`raise` are effects, `delay` is a struct

Three R7RS control features have direct Turmeric counterparts, and using them
is what keeps `(scheme base)` from becoming a second runtime:

- **`parameterize` / `make-parameter`** -> `stdlib/dynvar.tur`. Dynamic scoping
  with the right shape is already there (DV0-DV2 landed; DV3, spawn-conveying,
  is the open one and `parameterize` does not need it).
- **`guard` / `raise` / `raise-continuable` / `with-exception-handler`** ->
  algebraic effects. `raise-continuable` is exactly a performable effect whose
  handler resumes, and `raise` is one whose handler does not, which is a
  notably better fit than the usual try/catch encoding.
- **`delay` / `force` / `delay-force` / `make-promise`** -> a `Promise` struct
  (D3) with a memoized thunk. `delay-force`'s iterative-space guarantee needs
  the tail-call answer from D6; note the dependency rather than discovering it.

### D11 -- the gate

An `EXPERIMENTS[]` row named `r7rs`, all seven fields populated, `plan_path`
pointing here, `opt_global` at `g_opt_r7rs`. **The `#lang` line is itself the
enable**, per the Saffron plan's D9 -- a user who writes `#lang r7rs` has opted
in, and requiring `--enable=r7rs` as well is ceremony.

`LangBaseDescriptor.experiment` gets its first non-NULL value, so `tur dialects`
and the playground picker **badge** the row rather than hiding it. That is what
the field was built for (3.7) and using it as designed is how we find out
whether the design was right.

Per CLAUDE.md and the standing correction: `expires_at` is **advisory and never
blocks a release cut**. This row will be long-lived and is therefore the most
likely one to be misread that way; R4 in Section 7 exists to say so.

---

## 5. Stages

Each stage is independently landable and leaves the tree green-ish. The
interpreter leads, mirroring Saffron.

### R0 -- de-Saffronize the dynamic substrate (small; mechanical)

No behavior change. This is the stage that makes every later one cheap.

- `lang_span_is_saffron` -> `lang_span_is_dynamic`, backed by `LangTraits.dynamic`.
- `g_opt_saffron` -> `g_opt_dynamic_any`, set by `lang_dialect_apply` for any
  dynamic language.
- `Elab.toplevel_saffron` -> `toplevel_dynamic`.
- The ~45 elaboration sites and ~6 emitter sites convert; the ~20
  genuinely-Saffron surfaces (`--lang saffron`, `tur init --saffron`, the
  prelude hook) keep their names.
- **Exit criterion: the full suite is byte-identical before and after**, and
  `tests/fixtures/*` regenerates no snapshots. If a snapshot moves, the rename
  was not a rename.

> **What shipped (2026-09-23).** Exactly the list above. `lang_traits(d)`
> returns a `LangTraits` row (`name`, `default_reader`, `reader_axis_free`,
> `dynamic`, `prelude`, `experiment`) from a table in `lang_dialects.c`;
> `lang_span_is_dynamic` reads `->dynamic`, `lang_dialect_apply` sets
> `g_opt_dynamic_any` off the same bit, and `Elab.toplevel_dynamic` is the
> renamed field. All 154 `expected.c` snapshots regenerated byte-identical.
> One thing the list did not say and the code did: the prelude hook,
> `g_saffron_prelude` (a bool), became `g_lang_prelude` (the trait's stdlib
> tail, or NULL) in R1, so `stdlib_autoload.c` and `turi/preload.c` read the
> trait rather than a name. `tur repl --lang saffron`, `tur init --saffron`
> and `g_repl_start_saffron` keep their names as planned.

### R1 -- the `#lang` axis and the Scheme reader (medium)

- `LANG_R7RS`, `READER_R7RS`, `LANG_BASES[]` replacing the cross-product (D1).
- `lang_base_from_name` learns `r7rs`. `tur dialects` prints 9 rows.
- Reader variant, as a flag on `Reader` beside `neoteric_enabled`:
  `#t`/`#f`/`#true`/`#false`, `#\c` and the named chars, `#(...)`, `#u8(...)`,
  `` ` ``/`,`/`,@`, dotted pairs, `|sym with spaces|`, the `#e`/`#i`/`#x`/`#o`/
  `#b`/`#d` numeric prefixes, and Scheme string escapes (`\xHH;`, `\a`, the
  line-continuation form).
- **The `READER_TURMERIC` sweep** (2.6): grep every site, justify each.
- `tests/fixtures/r7rs-reader-*`, one per lexical feature.
- Exit criterion: a `#lang r7rs` file reads, and elaborates exactly as the
  equivalent `#lang saffron` file does. No Scheme semantics yet.

> **What shipped (2026-09-23).**
>
> - `LANG_R7RS`, `READER_R7RS`, and `LANG_BASES[]` -- a real table of nine
>   rows replacing the `DIALECTS[] x READERS[]` product. `lang_base_from_name`
>   in the reader is now `lang_base_lookup` over that table (plus the
>   `sweet-exp` alias), so a base that can be listed can be named and vice
>   versa. `tur dialects` prints `r7rs  r7rs  scheme  experimental (r7rs)`;
>   `--json` carries an `"experiment":"r7rs"` key on that row only;
>   `LangBaseDescriptor.experiment` has its first non-NULL value and the
>   playground picker badges the row (`tests/wasm_glue_lang_unit.c`).
>   `#lang r7rs/sweet` is TUR-E0331 (`errors/lang-r7rs-no-reader-axis`).
> - The `r7rs` `EXPERIMENTS[]` row (introduced 0.52.0, `expires_at` 0.70.0 --
>   advisory, per Section 7 R4), `g_opt_r7rs`, and D11's gate:
>   `lang_dialect_apply` calls `experiment_enable("r7rs", XF_SRC_CLI)` then
>   `experiment_warn_if_used`, so the directive is the enable and TUR-W0060
>   prints once per compile.
> - The reader variant, as `Reader.scheme_enabled` (plus `fold_case`):
>   `#t`/`#f`/`#true`/`#false`; `#\c` with `alarm` added to the shared name
>   table, `#\x<hex>` to any scalar value, and a multi-byte UTF-8 `#\<char>`;
>   `#(...)` as F_VEC; `#u8(...)` as the call form `(bytevector ...)` (no
>   bytevector Form -- R3 gives it a meaning); `,`/`,@` as unquote (`~`
>   becomes an identifier character, as do `%` and a non-initial `@`);
>   dotted pairs -- the reader validates the shape and keeps the `.` symbol
>   as the improper-tail marker, since the Form model has no improper list;
>   `|sym|` with the string escapes; `#x`/`#o`/`#b`/`#d`/`#e`/`#i` in either
>   order; `+5`, `.5`, `-.5`, `+inf.0`/`-inf.0`/`+nan.0`; the R7RS string
>   escapes (`\a`, `\b`, `\|`, `\x<hex>;`, the line continuation);
>   `#!fold-case`/`#!no-fold-case`. `#e` on a non-integral literal is a
>   diagnostic naming R5 (`errors/r7rs-reader-exact-rational`) rather than a
>   silent 1.5. Curly-infix stays on (SRFI-105 is a Scheme SRFI); neoteric
>   stays off; keywords, `[...]`, `#map{...}`, inline C and `^tailcall` all
>   still read, and `true`/`false`/`nil` are still literals -- that last one
>   is R2's call, when the truthiness predicate lands, not the reader's.
> - Fixtures: `r7rs-reader-lexemes` (runtime, both back ends),
>   `r7rs-reader-forms` (a `tur parse-check` pair, Scheme spelling against
>   Turmeric spelling, form for form), `r7rs-elaborates-as-saffron` (the
>   exit criterion: identical emitted C and identical stdout compiled and
>   interpreted against the `#lang saffron` twin), and the three `errors/`
>   pins.
> - **The `READER_TURMERIC` sweep** (2.6), every site, with its verdict:
>
>   | Site | Verdict |
>   |---|---|
>   | `main.c` resolve_reader_type (entry file) | runs `detect_lang_dialect`; correct |
>   | `main.c` project-prelude loader (~1145) | followed by `tur_source_file_apply_lang_header`; correct |
>   | `main.c` cmd_format / parse_check_read | honour a directive when present; the forced reader is the no-directive default; correct |
>   | `main.c` cmd_fmt `--lang` | knows `turmeric`/`sweet`/`curly-infix`/`neoteric` only; an `r7rs` value is R9's (fmt needs a Scheme printer, and `form_print` would write `#t` as `true` today) |
>   | `main.c` explain snippets (~10708), `web/tutorial.c` | synthetic Turmeric text; correct |
>   | `elab_toplevel.c` `(load ...)` (~1106) | runs detection; correct |
>   | `elab_module.c` `(import ...)` (~441) | runs detection (the S7 fix); correct |
>   | `stdlib_autoload.c` (~216) | followed by `apply_lang_header`; correct |
>   | `turi/env.c` default, `turi/eval.c` (13719, 14043) | `turi_eval` detects an inline `#lang` and sets `env->lang`; correct |
>   | `compiler/reader.c` reader_macros_load_file (3619) | a `#use-reader-macros` file is read with no detection, so a directive in one is "unexpected character '#'"; harmless today (no such file carries one) and noted, not fixed |
>   | `web/wasm_lsp.c` (~147) | **the third one.** Calls `detect_lang`, not `detect_lang_dialect`, and never sets `file.lang`, so the browser LSP elaborates a `#lang saffron` (or `r7rs`) buffer as Turmeric and reports spurious `int`-default diagnostics. Not fixed here -- it is the LSP row of R9 and needs its own test -- but it is exactly the shape 2.6 predicted, found by the sweep rather than by a user |
>   | `lsp/lsp.c` (2111) | uses `reader_type_from_extension` only; same R9 item, measure before assuming |

### R2 -- core forms, interpreter (medium)

`define`, `lambda`, `let`/`let*`/`letrec`/`letrec*`, named `let`, `do`, `begin`,
`set!`, `if`, `cond` (including `=>`), `case`, `and`/`or`, `when`/`unless`,
`case-lambda`, `define-values`/`let-values`/`let*-values`.

Most map onto existing Turmeric forms or onto `defmacro`. The ones that do not
are `case-lambda` (arity dispatch on a dynamic callee) and the `-values` family
(D-open: see Section 8).

Truthiness is the one place R7RS and Saffron **disagree**, and it must be
handled rather than inherited: Saffron's D4 settled that `false` and `nil` are
falsy while `0`, `""` and empty containers are truthy. **Scheme is stricter --
only `#f` is false.** `0` and `'()` are both true in Scheme, and `'()` being
true is a classic source of bugs in ports from other Lisps. So `#lang r7rs`
needs its own truthiness predicate, not Saffron's. One trait field, one
`__tur_dyn_truthy` variant.

Exit criterion: a named-`let` loop summing a list runs under `--interpret`.

### R3 -- data, and the Turmeric seam (large)

**The seam is here, not at the end.** This is the stage that proves the point
of the exercise, and if it does not work we want to know before writing
`(scheme base)`.

- The `Pair` type, mutable, with `TUR_REGION_NOTE` on both setters and the
  `region-escape-via-store` case (D4).
- `quote` as a static datum constructor (D4), and `quasiquote`/`unquote`/
  `unquote-splicing` over it.
- `Char`, `EofObject`, and the rest of D3's table.
- Vectors over `Vec`, bytevectors over a `:uint8` buffer, strings: note that
  R7RS strings are **mutable** (`string-set!`) while `stdlib/string.tur`'s
  `String` is immutable and refcounted -- so Scheme strings are a distinct type
  with a `String` conversion, not a reuse. Say so before building.
- `eq?` / `eqv?` / `equal?`, with `equal?` terminating on cycles.
- **`define-library`, `import`, and the `(turmeric ...)` head** (D9).
- `tests/run-r7rs-import.sh`, both directions, both back ends.

Exit criterion: the snippet in D9 runs -- a Scheme file calling `stdlib/hamt`
and getting the right answer.

### R4 -- `syntax-rules` (large)

Pattern matching with `...` at any depth, nested ellipses, literals,
improper-list patterns, vector patterns; template instantiation; renaming
hygiene (D5a); `define-syntax`, `let-syntax`, `letrec-syntax`,
`syntax-error`. `er-macro-transformer` as the documented low-level escape.

Exit criterion: the standard's own `syntax-rules` definitions of `or`, `let*`
and `do` expand correctly, **plus** a named failing test that demonstrates the
referential-transparency gap (D5).

### R5 -- numbers (medium)

Exactness, the predicate set (`exact?`, `inexact?`, `exact-integer?`,
`nan?`, `infinite?`, `finite?`), checked exact overflow (D8), `number->string`
and `string->number` across radixes, and the arithmetic surface. `Rational` and
`Complex` offered with their limits documented.

Leading probe is `7.1`, never `7.0` and never `7`, per CLAUDE.md -- and in this
stage that rule is load-bearing rather than procedural, because the whole stage
is about exact/inexact divergence.

### R6 -- control (large; contains the hardest item)

Proper tail calls on the compiled path -- **T6 of
[proper-tail-calls-plan.md](proper-tail-calls-plan.md)**, which is a
prerequisite landing on its own schedule, not work done here (D6). `dynamic-wind`. `values` and
`call-with-values`. `guard`/`raise`/`raise-continuable`/`with-exception-handler`
over effects (D10). `parameterize` over `dynvar` (D10). `delay`/`force`
(D10). `call/cc` at the escape level, with the re-entrant case named and
scheduled (D7).

`dynamic-wind` interacting correctly with re-entrant continuations is gated on
the same item and should be tested as a pair, since a `dynamic-wind` that is
correct only for escapes is a trap.

### R7 -- `(scheme base)` and the other eleven libraries (large; parallelizable)

`base`, `char`, `complex`, `cxr`, `eval`, `file`, `inexact`, `lazy`, `load`,
`process-context`, `read`, `repl`, `time`, `write`, `case-lambda`.

**Adaptors, not reimplementations**, on the Saffron prelude's R5 rule: a
procedure that forwards to a typed stdlib entry point is right; one that
reimplements an existing typed operation because the typed signature needs
annotations is right; one that implements something the typed stdlib does not
have at all **belongs in the typed stdlib first**.

`(scheme eval)` and `(scheme repl)` need an evaluator at runtime, which means
either linking `libturi` into the emitted program or declaring them
interpreter-only. That is Section 8's question, not a settled decision.

### R8 -- ports and I/O (medium)

The port taxonomy, string ports, `read`, `write`, `display`, `write-shared` and
`write-simple` (which need cycle detection, so they pair with R3's `equal?`).

### R9 -- tooling (medium; parallelizable)

`tur repl --lang r7rs`, `tur fmt` preserving `#lang r7rs` and formatting Scheme,
`tur init --r7rs`, the LSP (which 2.6 suggests works by inheritance -- **measure
it, do not assume it**, since that is exactly how `tur fmt` and module import
were both caught), editor packs, `tools/gendocs.py`, and
`docs/guides/r7rs-guide.md`.

### R10 -- conformance (medium, continuous)

Run a real suite rather than hand-writing 400 fixtures. chibi-scheme's
`tests/r7rs-tests.scm` is the de facto standard, is a single file, and is
permissively licensed. Wire it as one ctest target that **reports a pass count**
from R4 onward, so the number moves visibly across stages instead of arriving
as a verdict at the end.

Per R3 in Section 7, this is the fixture-count mitigation as much as it is the
conformance story.

---

## 6. Carve-outs

Permanent, or permanent-until-named-epic. These are the honest answer to "what
does R7RS-on-Turmeric give up".

The Saffron plan's carve-out table was **wrong in four rows until it was
measured**, always in the direction of assuming that a static guarantee cannot
survive a dynamic default. The correction was that the distinction is whether a
proof reads an *inferred* type (regions do) or an *annotation* / a walk over
uses and scopes (everything else does). R7RS inherits that corrected table, and
this one should be **measured the same way** before it is believed.

| Feature | Status under `#lang r7rs` | Why |
|---|---|---|
| `with-region` / regions | rejected, TUR-E0312 | the proof reads the bracket's inferred result type; expected to inherit Saffron's carve-out |
| Monomorphization, by-value HKT | off; everything boxes | needs ground types at each site |
| Refinement types | runtime contracts | no static base type to discharge over |
| Linear / affine / unique, borrows, session types, GADTs | **expected to survive**, via annotations | Saffron measured these as kept; R7RS has no *syntax* for the annotations, so this is "survives if written in an annotated Turmeric module and called across the seam" |
| Full numeric tower | int64 exact + checked overflow | D8; bignums are a named epic |
| Re-entrant `call/cc` | not at first | D7; the conformance claim is gated on it |
| `(scheme eval)`, `(scheme repl)` | interpreter-first | needs an evaluator at runtime |
| Typeclass dispatch on `any` | inherits Saffron's S9 state | separate epic |

---

## 7. Risks

**R1 -- two dynamic substrates.** The thesis is that R7RS is Saffron plus a
reader plus a prelude. The failure mode is an `if (lang == LANG_R7RS)` sitting
*beside* a `LANG_SAFFRON` test rather than replacing it. Mitigation: R0 lands
first and converts the shared sites to a trait; after that, a new
`LANG_R7RS`-specific branch in the elaborator is a design smell that should be
argued for in review, not added quietly.

**R2 -- conformance is a cliff.** Ninety percent of R7RS is ten percent of the
work. The remaining ten percent -- re-entrant `call/cc`, `dynamic-wind` under
re-entry, hygiene corners, exact arithmetic, cyclic `equal?`/`write` -- is most
of it. Mitigation: R10 runs a real suite from R4 and publishes a number, so the
distance to done is measured continuously rather than estimated once.

**R3 -- fixture count.** The suite is ~1442 fixtures at ~4-5 minutes. Saffron
added ~70. A per-procedure fixture policy for R7RS would add several hundred and
push the suite past its useful wall-clock. Mitigation: R10's external suite is
**one** ctest target; hand-written fixtures are reserved for reader features,
seam behavior, and regression pins.

**R4 -- `expires_at` misread as a gate.** CLAUDE.md is explicit that an expiring
`EXPERIMENTS[]` row never blocks a release cut, and that believing otherwise has
already stranded two releases. An `r7rs` row will be long-lived and gated for
longer than any row before it, which makes it the likeliest to be misread. It is
advisory. Graduating early is routine; bumping it with a one-line rationale is
routine; refusing a version bump over it is a bug.

**R5 -- this does not move v1.** CLAUDE.md is clear that the project is on one
track to v1. This is a multi-quarter epic that is orthogonal to that track. It
should be scheduled as a deliberate decision to spend time off the track, not
smuggled in as incremental work -- and R0 and R1 are cheap enough to land
independently if the answer is "later".

**R6 -- performance expectations.** A Scheme on a substrate with no generational
GC and no NaN-boxing will allocate harder than a Scheme designed for one.
Saffron's R2 risk applies verbatim. Mitigation: an `r7rs` row in `benchmarks/`
from R3 onward, not from R9, so the number exists before anyone forms an
expectation from a demo.

---

## 8. Open questions

1. **Multiple values.** Turmeric has no `values`/`call-with-values`. Options: a
   `Values` struct carrying a list (simple, allocates), a multiple-return
   convention in the ABI (fast, invasive), or restricting to the common
   `let-values` pattern and desugaring it away. Not decided; R6 needs it and
   R2's `define-values` wants it early.
2. **Static-datum mutation detection** (D4). How does `set-car!` cheaply refuse
   a `.rodata` pair -- an address-range test, a spare tag bit, or a
   debug-build-only check? R7RS says "it is an error", which permits undefined
   behavior, but silently corrupting `.rodata` is a bad answer even when the
   standard allows it.
3. **`(scheme eval)` in compiled programs.** Link `libturi` into every emitted
   Scheme program, link it on demand when `(scheme eval)` is imported, or
   declare `eval` interpreter-only? The third is honest and cheap; the second is
   the right answer and needs a mechanism.
4. **File extension.** `.scm` is the obvious spelling and means every tool
   learns a new file type. The Saffron plan deferred `.saf` for exactly this
   reason, and R7RS should defer `.scm` the same way -- `#lang r7rs` inside a
   `.tur` file until the semantics settle.
5. **Does `#lang r7rs` get the reader axis at all?** D1 says no
   (`reader_axis_free = false`). But `r7rs/sweet` is arguably meaningful --
   sweet-expressions were designed for Scheme, and SRFI-110 is a Scheme SRFI.
   Recorded as a deliberate deferral rather than an oversight; if it is ever
   wanted, the `LangTraits` table is the place it goes.
6. **Which R7RS?** R7RS-small is the target. R7RS-large is a moving set of
   dockets and is explicitly out of scope; if it is ever wanted it is a sibling
   base token (D1), not a flag.

---

## Appendix A -- probe transcript

All against `./build/tur` at v0.50.0, Debug build, on 2026-09-21 (macOS,
arm64). These are the measurements Sections 2 and 3 cite.

### A.1 -- the tail-call matrix (3.1)

Full transcript, including the two wrong turns this probe took first, is in
[proper-tail-calls-plan.md](proper-tail-calls-plan.md) Appendix A. Depth is read
from the environment in every probe; an earlier version passed it as a literal
and measured only clang's constant folding.

```
pA  self          -O2 n=10000000: 0        -O0 n=10000000: 0
pB  mutual x2     -O2 n=10000000: true     -O0 n=10000000: CRASH (exit=139)
pC  mutual x8     -O2 n=10000000: 0        -O0 n=10000000: CRASH (exit=139)
pD  indirect      -O2 n=10000:    1        -O2 n=1000000:  CRASH (exit=139)

indirect tail call: deepest OK ~= 29335, crashes by ~= 33202   (8 MB stack)
```

Under `--interpret`, the indirect probe passes at 1e6.

### A.2 -- `quote` is not a data constructor (3.2)

```
$ tur run q.tur                        # (let [s (quote foo)] (println s))
error [TUR-E0006]: operator lookup failed for 'println':
                   got 1 arg(s), first arg type Sym

$ tur run q2.tur                       # (let [s (quote (1 2 3))] ...)
error: expression in call head has type `int`, which is not callable

$ tur check dotted.tur                 # (quote (a . b))
error: unbound symbol '.'
```

The first is a stdlib gap (no `println` overload for `Sym`); the second and
third are representation gaps.

### A.3 -- three Scheme lexical features already work (2.2)

```turmeric
(defn main [] : int
  (println 1) #| block |# (println 2)
  #;(println 99)
  (println 3)
  0)
```

```
$ tur run lex.tur
1
2
3
```

`#|...|#` and `#;` both behave. By contrast:

```
$ tur check t.tur                      # (println (quote #t))
error: unexpected character '#' (0x23)
```

And `,` is whitespace -- `(println 1 , 2)` reports "got 2 arg(s)", so the comma
was consumed as separator, not as a form.

### A.4 -- the dialect blast radius (2.1)

```
$ grep -rn "LANG_SAFFRON\|lang_span_is_saffron\|g_opt_saffron" src/ \
    --include=*.c --include=*.h | grep -v generated | wc -l
71
```

19 files. The canonical site is `elab_fns.c:5649`:

```c
return lang_span_is_saffron(sp) ? TY_ANY : TY_INT;
```

### A.5 -- the base axis is a cross-product (3.7)

```
$ tur dialects
BASE                   LANGUAGE  READER       STATUS
turmeric               turmeric  s-expr       stable
turmeric/curly-infix   turmeric  curly-infix  stable
turmeric/neoteric      turmeric  neoteric     stable
turmeric/sweet         turmeric  sweet        stable
saffron                saffron   s-expr       stable
saffron/curly-infix    saffron   curly-infix  stable
saffron/neoteric       saffron   neoteric     stable
saffron/sweet          saffron   sweet        stable

8 `#lang` base dialects.
```

`lang_bases_count()` returns `|DIALECTS| * |READERS|`, and
`LangBaseDescriptor.experiment` is `NULL` on all eight rows.

---

## See also

- [proper-tail-calls-plan.md](proper-tail-calls-plan.md) -- D6's prerequisite,
  with the full tail-call measurement matrix
- [saffron-lang-plan.md](saffron-lang-plan.md) -- the dynamic substrate this
  plan inherits, and the staging discipline it copies
- [docs/guides/saffron-guide.md](../guides/saffron-guide.md)
- [docs/guides/delimited-control-operators-guide.md](../guides/delimited-control-operators-guide.md) -- `call/cc`, `call/cc*`, `shift`/`reset`
- [docs/guides/macros-guide.md](../guides/macros-guide.md) -- `defmacro`, manual hygiene, `defmacro*`
- [docs/archive/macro-system-direction-plan.md](../archive/macro-system-direction-plan.md) -- why there is no phase tower, and the `Syntax` substrate
- [docs/guides/module-system-guide.md](../guides/module-system-guide.md)
- [docs/guides/numeric-tower-guide.md](../guides/numeric-tower-guide.md)
- [docs/guides/performance-guide.md](../guides/performance-guide.md) -- the self-tail-call boundary that A.1 measures against
