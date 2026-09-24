# R7RS-small as a `#lang` over the Turmeric runtime

Status: **R0 through R9 landed 2026-09-23/24.** `#lang r7rs` is a base
(`LANG_R7RS` + `READER_R7RS`, ninth row of `LANG_BASES[]`), the `r7rs`
`EXPERIMENTS[]` row gates it with the directive as its own enable, the Scheme
reader variant reads every lexeme R1 lists, and R2's core forms -- `define`,
`lambda`, the `let` family and named `let`, `do`, `begin`, `set!`, `if`,
`cond`, `case`, `and`/`or`, `when`/`unless`, `case-lambda`, the `-values`
family -- lower onto Turmeric's own forms (`src/compiler/scheme_lower.c`)
with Scheme truthiness, over a `stdlib/r7rs/prelude.tur` of core procedures.
R3 gives the data its types (mutable pairs, the null and eof singletons,
chars, vectors, bytevectors, records, `quote`/`quasiquote`, `equal?` on
cycles) and opens the Turmeric seam in both directions: `define-library`,
`import` with `only`/`prefix`/`rename` and the `(turmeric ...)` head, pinned
by `tests/run-r7rs-import.sh` on both back ends. The D9 exit criterion (a
Scheme program calling the stdlib map and getting the right answer) is
`tests/fixtures/r7rs-stdlib-seam`. R4 adds `syntax-rules` -- `define-syntax`,
`let-syntax`, `letrec-syntax`, `syntax-error`, the full pattern language and
D5's renaming hygiene -- as an expander inside the same lowering pass; the
standard's own `or`, `let*` and `do` expand correctly
(`tests/fixtures/r7rs-syntax-rules`, `r7rs-syntax-rules-do`) and the
referential-transparency gap is the named failing test
`r7rs-syntax-rules-referential-transparency` under a new `expected.xfail`
marker. R5 gives the numbers their R7RS semantics over int64 and double:
`(+)`, `(- x)`, comparison chains, promotion of a mixed literal pair,
inexact `/` when the quotient is not exact, checked exact overflow (D8), the
exactness predicates and conversions, rounding, integer division, `expt`,
`sqrt`, the transcendental set, radix `number->string`/`string->number` and
R7RS float spelling (`tests/fixtures/r7rs-numbers`). R6 gives the control
forms: `call/cc` at the escape level (D7), `dynamic-wind`, `guard`/`raise`/
`raise-continuable`/`with-exception-handler` and error objects,
`parameterize`, `delay`/`delay-force`/`force`, and -- the largest item --
the compiled back end's dynamic-closure gaps are closed, so every r7rs
fixture runs on both back ends and a Scheme procedure call is a proper
tail call 10,000,000 deep at `-O0` (`tests/fixtures/r7rs-control`,
`r7rs-tail-calls`). R7 completes the R7RS-small libraries short of
ports: the rest of `(scheme base)`, `(scheme char)`, `(scheme cxr)` and
`(scheme complex)` in the prelude, and `(scheme time)`, `(scheme
process-context)` and the non-port half of `(scheme file)` as files spliced
in only when imported (`tests/fixtures/r7rs-base-library`,
`r7rs-system-libraries`); `(scheme eval)`, `(scheme repl)`, `(scheme load)`
and `include` are refused with their reason. R8 gives the ports: string,
bytevector and file ports over one C buffer, the current ports as parameter
objects, the whole R7RS I/O surface, `write`/`display` with datum labels for
cycles, `write-shared`/`write-simple`, and `(scheme read)`
(`tests/fixtures/r7rs-ports`, `r7rs-write-labels`, `r7rs-read`,
`r7rs-file-ports`). R9 gives the tooling: `tur repl --lang r7rs`, a `tur fmt`
that re-indents Scheme and never reprints a token, `tur init --r7rs`, the LSP
(native and browser) analysing and formatting Scheme, the editor packs,
`gendocs` reading Scheme definitions, and `docs/guides/r7rs-guide.md`. R10
(conformance) is unbuilt. Each landed stage carries a "What shipped" note
below.

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

> **What shipped (2026-09-23).**
>
> - **The lowering is a Form -> Form pass**, `src/compiler/scheme_lower.c`,
>   run after `(load ...)` expansion in both the entry program
>   (`elab_toplevel.c`) and an imported module (`elab_module.c`), keyed
>   per form on the span's file being `LANG_R7RS`. "Most map onto existing
>   Turmeric forms" turned out to be all of them: `define` -> `defn`/`def`,
>   `lambda` -> `fn`, `let` -> `let` (with temporaries when an init mentions
>   a binder, since Turmeric's `let` is sequential), named `let`/`do`/`letrec`
>   -> `letrec`, `begin` -> `do` (spliced at top level), `cond`/`case`/`and`/
>   `or`/`when`/`unless` -> `if` chains (Scheme's value-returning `and`/`or`,
>   not Turmeric's bool ones), `case-lambda` -> a variadic `fn` dispatching on
>   `length`, and the `-values` family over a `Values` carrier. Internal
>   defines follow R7RS 5.3.2: a run of lambdas becomes one `letrec`, a value
>   a `let`, in letrec* order. Turmeric-shaped forms in a Scheme file pass
>   through with their subforms lowered, so the R1 fixtures still run. No
>   `defmacro` was needed, and no `if (lang == LANG_R7RS)` was added to the
>   elaborator: the two per-file decisions it makes (truthiness and the
>   static-condition rule below) read `LangTraits.scheme_truthiness`.
> - **Truthiness is a trait**, `scheme_truthiness`, with its own reserved
>   operator (`SCHEME_TRUTHY_OP`, `__tur_dyn_truthy_scheme` in the emitted
>   preamble, a second arm in `eval.c`) chosen by `elab_saffron_truthy` per
>   file. The rule R7RS 6.3 states is also a compile-time fact: in a Scheme
>   file an `if` whose condition has any STATIC type other than `bool` is
>   decided at elaboration (`nil` false, everything else true), which is what
>   makes `(let ((x 5)) (if x ...))` legal under Saffron's local inference
>   (D3) at no runtime cost.
> - **The substrate needed two things, both Saffron gaps too**: `set!` into an
>   `any` cell widens a concrete value (elab_forms.c), and a `fn` parameter
>   vector does not take `^mut` (it reads as an extra parameter and the call
>   partially applies -- found by the mutual-recursion probe printing `fn`), so
>   a `set!` parameter is rebound as a mutable local inside the body.
>   Mutability is decided lexically, per binding body, not file-wide.
> - **`stdlib/r7rs/prelude.tur`**, read as `#lang r7rs` itself and written in
>   Turmeric shapes: the list procedures over the `(Cons any)` chain Saffron's
>   widen builds (`'()` reads as `(list)`, which is the null cell on both back
>   ends), `eqv?`/`eq?`/`equal?`, the type and numeric predicates, `display`/
>   `write`/`newline` on three newline-free inline-C write primitives with
>   interpreter natives, and `values`/`call-with-values`/`apply` (up to four
>   arguments -- the compiled dynamic-call helpers' limit). Every procedure is
>   spelled `r7rs-<name>` and reached through the pass's rename table, because
>   `car`, `list`, `length`, `min`, ... are auto-loaded stdlib names that a
>   top-level definition cannot shadow. R7's `(import (scheme base))` maps
>   onto the same table.
> - **Section 8 Q1 is decided**: multiple values are a `Values` carrier over a
>   list, `(values x)` is `x` itself (no allocation, and a one-value producer
>   works with `call-with-values` unchanged), `let-values` and friends
>   desugar to `values-ref` reads. The ABI convention was not needed.
> - **Deviations at R2**, all named in the fixtures: a top-level redefinition
>   of a prelude name (`(define (car x) ...)`) is the Turmeric stdlib-name
>   error, not a shadowing (R7RS 5.3.1); an operator is not a first-class
>   value (`(apply + ...)` waits on R5's `+` procedure); `(define (main) ...)`
>   is the program entry and returns 0; a lambda define that is later `set!`
>   becomes a `let` and loses self-reference by name; `true`/`false`/`nil`
>   stay literals under the reader (`nil` is a true value under Scheme
>   truthiness, as it should be).
> - **The compiled back end** runs everything in `tests/fixtures/r7rs-core-forms`
>   (definitions, the conditionals, `let`/`let*`, `set!` on globals and
>   closed-over locals, internal defines, the list procedures, display/write)
>   but not a letrec-bound closure over `any` that calls itself (named `let`,
>   `do`, `letrec`), a dynamic call through `apply`, nor `for-each` with a
>   two-statement lambda -- `docs/archive/r7rs-compiled-dynamic-shapes.md`
>   has the repros. Those are the plan's interpreter-first staging in action
>   and R6's to close; `r7rs-core-forms-interp` and `r7rs-named-let-sum` carry
>   `requires.interp-only` (they are `tests/run-turi.sh`'s) until then.

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

> **What shipped (2026-09-23).**
>
> - **D3's table, in `stdlib/r7rs/prelude.tur`**: a pair is `R7rsPair`, a
>   heap struct of two `any` fields (`a`/`d` -- `car`/`cdr` as field names
>   collide with the stdlib's `car` in `.field` method lookup); the empty list
>   and the eof object are one-field singleton structs with one process-wide
>   value each; a char is an opaque over the Unicode scalar value, so
>   `char?` and `integer?` are disjoint; a vector is `(Vec any)`; a
>   bytevector wraps a `(Vec int)`; a symbol is `Sym`, with `string->symbol`
>   interning at runtime through `stdlib/sym-dynamic.tur`. Every predicate
>   is a plain function over `is?`, not an if-guard narrowing, because a
>   narrowed heap value is unique under the linearity rules and the second
>   use was "cannot copy unique value".
> - **A rest parameter is a Scheme list.** A variadic `& r : any` arrives as
>   the `(Cons any)` chain Saffron's widen builds; the lowering converts it
>   at the one place it enters Scheme code (`rebind_rest`), so `(define (f
>   . xs) (length xs))` and `case-lambda`'s rest clauses see `R7rsPair`s
>   like every other list. R2's prelude walked the chain directly; from R3
>   the chain is an implementation detail of the seam.
> - **`quote` builds at runtime, not in `.rodata`** -- a deviation from D4's
>   static table. The datum walker in `scheme_lower.c` lowers a quoted datum
>   to `r7rs-list`/`r7rs-cons`/`r7rs-vector`/`r7rs-char__` calls, so `'(1 2
>   3)` in a loop allocates, and mutating a literal is not detected. D4's
>   other half holds: `set-car!`/`set-cdr!` are `(set! (.a p) v)`, the
>   region-noted field store (the emitted body carries
>   `TUR_REGION_NOTE_WORDS`), and `tests/fixtures/region-escape-via-store`
>   case 10 is that shape -- a widened word stored into a heap cell's field
>   inside a bracket and read after it. `quasiquote` is depth-aware
>   (`unquote` at depth 1 only, nested quasiquote re-quoted), `unquote-
>   splicing` uses `append`, and a quoted vector is `list->vector` of the
>   walked elements.
> - **Strings stay `cstr` and are immutable.** The plan asked for a distinct
>   mutable Scheme string with a `String` conversion; this stage declines
>   `string-set!`/`string-fill!` and says so rather than aliasing. `string-
>   length`/`string-ref`/`substring`/`string-append`/`string<?`/`string->
>   list`/`number->string` are newline-free inline-C primitives with
>   interpreter natives; `eqv?` on two strings compares content (there is
>   no identity to compare -- a literal is a pointer into `.rodata` on one
>   back end and an interned value on the other).
> - **`equal?` terminates on cycles** with a `seen` list of pair-pairs under
>   comparison (the coinductive reading, R7RS 6.1) rather than union-find;
>   `eqv?`/`eq?` are one predicate (chars and small ints are immediate).
>   `define-record-type` lowers to a `defstruct` plus the named constructor,
>   predicate, accessors and modifiers; `cond-expand` evaluates
>   `and`/`or`/`not`/`library`/`else` over the features `r7rs`, `turmeric`
>   and `exact-closed` (`(library (scheme ...))` and an auto-loaded stdlib
>   module hold).
> - **The seam (D9)**: `(define-library (a b) (export ...) (import ...)
>   (begin ...))` is `(defmodule a/b ...)`; a program with imports is
>   wrapped in a synthesized module with a synthesized `main`; `(turmeric
>   x/y)` is `(import x/y)`, `(only ...)` is `:refer`, `(prefix ...)` and
>   `(rename ...)` are rename-table entries over the same import, `(scheme
>   base)` and its siblings map onto the prelude, `(include ...)` is
>   `load`. `(except ...)` is a diagnostic naming `(only ...)` (Turmeric's
>   import has no "all but"; `tests/fixtures/errors/r7rs-import-except`).
>   An imported `#lang r7rs` module gets the prelude on the import path
>   (`elab_module.c`) when the entry program is not itself Scheme.
>   `tests/run-r7rs-import.sh` (ctest `tur_r7rs_import`) runs a Turmeric
>   module importing a `define-library`, and a Scheme program importing a
>   Turmeric module with `only`/`prefix`/`rename` and the stdlib through
>   `(turmeric stdlib/vec)`, on both back ends.
> - **A substrate bug the seam surfaced, fixed for Saffron too**: a callee
>   with a COMPOUND parameter type (`[v : (Vec any)]`) defined BELOW its
>   caller was forward-declared with the `int` placeholder, and the
>   Saffron seam then unboxed the `any` argument to int -- "cast: any holds
>   Vec, not int" at runtime, C passing an int64 to a `tur_adt_Vec__any *`.
>   The pass-1 forward declaration now carries the full type of a closed
>   compound parameter (and, in the `defmodule` pre-pass, a closed compound
>   return) in a dynamic file (`elab_fwd_param_full_types`,
>   `elab_fwd_compound_result_type`); typed files keep their placeholders
>   and int64 hatches. `tests/fixtures/saffron-fwd-decl-app-param-seam`
>   pins both orders.
> - **Deviations and gaps at R3**: a generic stdlib constructor with no
>   value arguments (`(map-new)`) cannot be grounded from Scheme -- `K`/`V`
>   never bind -- so the seam fixture builds its map from a `#map{}`
>   literal. That is a Saffron gap, pre-existing on `main`, and it has two
>   faces (a static error reported inside stdlib/map.tur, and a compiled
>   panic where `--interpret` answers correctly when the map comes back
>   through an unannotated function):
>   `docs/reported/saffron-open-generic-result-not-grounded.md` has the
>   repros and the one-site fix direction; `apply` takes at most four
>   arguments (the compiled dynamic-call helpers' limit); `string-copy`
>   returns its argument (strings are immutable, so a copy is the value);
>   `vector-map`/`vector-for-each`/`string-map` and the char-class
>   procedures beyond `alphabetic?`/`numeric?`/`whitespace?`/`upcase`/
>   `downcase` wait on R7.
>   `tests/fixtures/r7rs-data-forms` runs every R3 shape on both back ends
>   with identical output.

### R4 -- `syntax-rules` (large)

Pattern matching with `...` at any depth, nested ellipses, literals,
improper-list patterns, vector patterns; template instantiation; renaming
hygiene (D5a); `define-syntax`, `let-syntax`, `letrec-syntax`,
`syntax-error`. `er-macro-transformer` as the documented low-level escape.

Exit criterion: the standard's own `syntax-rules` definitions of `or`, `let*`
and `do` expand correctly, **plus** a named failing test that demonstrates the
referential-transparency gap (D5).

> **What shipped (2026-09-23).**
>
> - **The expander lives in the lowering pass, not on `defmacro*`/`Syntax`**
>   -- a deviation from D5's substrate choice, for a reason D5 did not have in
>   front of it: R2 made the core forms a Form -> Form pass that runs BEFORE
>   elaboration, and a `defmacro*` runs inside elaboration, after it. A
>   `syntax-rules` written as a `defmacro*` would hand back Scheme forms
>   (`let`, `cond`, a named `let`) that nothing would lower. So `syntax-rules`
>   is a pattern matcher and template instantiator over forms in
>   `src/compiler/scheme_lower.c` (`sr_match`, `sr_inst`, `sr_expand`), and
>   every expansion is lowered on the spot. D5's other verdicts hold: no phase
>   tower, define-before-use, `gensym`-grade freshness for the renames.
> - **The pattern language is complete**: `_`, literals (matched by name),
>   `...` at any depth with nested ellipses, an element after an ellipsis
>   (`(_ a ... b)`), improper tails (`(_ a . rest)`), vector patterns, datum
>   literals, a custom ellipsis (`(syntax-rules dots (lits) ...)` -- a symbol;
>   `:::` reads as a keyword under this reader) and the `(... ...)` escape.
>   Templates substitute, iterate `x ...` and flatten `x ... ...`, splice a
>   substituted list into a dotted tail, build vectors, and `syntax-error`
>   reports at the use site. `define-syntax` at top level or body start (a
>   macro may expand to a `define`, R7RS 5.3.2), `let-syntax` and
>   `letrec-syntax` scope lexically; both are letrec-scoped because expansion
>   is lazy. A use whose expansion never stops is an error after 1000 steps
>   (`errors/r7rs-macro-expansion-loop`); no matching rule and a
>   `syntax-error` have their own fixtures.
> - **Hygiene is D5(a) renaming**: after instantiation, every identifier the
>   template introduced (not substituted from the use site) that lands in a
>   binding position -- `lambda`/`define` formals, the `let` family, named
>   `let`, `do`, `let-values`, `case-lambda` -- is renamed to a fresh symbol
>   consistently across that expansion, so `swap!`'s `tmp` and `my-or`'s
>   `x` cannot capture a use-site name; a quoted symbol keeps its name.
>   Introduced FREE identifiers keep their names, which is exactly the
>   referential-transparency gap D5 predicts, and it is on record as a test
>   rather than prose: `tests/fixtures/r7rs-syntax-rules-referential-transparency`
>   holds the R7RS answer (`(1)`), prints `#(1)` today, and carries the new
>   `expected.xfail` marker (both harnesses: the mismatch passes as `(xfail)`,
>   a match fails and says to delete the marker). D5(b) closes it.
> - **Two things the lowering needed**: the mutability scan runs before any
>   expansion, so a `set!` a template performs is accounted for syntactically
>   (a template that sets a pattern variable marks the macro as a setter of
>   its arguments; one that sets its own name marks that name), and the
>   reader's `()` (F_NIL) is accepted wherever a binding list or formals list
>   is expected, since a template can produce an empty one.
> - **Deviations at R4**: `er-macro-transformer` is deferred (it needs a
>   transformer procedure run at expansion time;
>   `errors/r7rs-er-macro-transformer-deferred` says so); macros do not cross
>   a `define-library` boundary (Turmeric modules export definitions, not
>   syntax); a local variable that shadows a macro name does not hide the
>   macro; `let-syntax` is `letrec-syntax`. The standard's `do` runs under
>   `--interpret` only (`r7rs-syntax-rules-do`, `requires.interp-only`): its
>   expansion is the letrec-bound lambda over `any` from
>   `docs/archive/r7rs-compiled-dynamic-shapes.md`.

### R5 -- numbers (medium)

Exactness, the predicate set (`exact?`, `inexact?`, `exact-integer?`,
`nan?`, `infinite?`, `finite?`), checked exact overflow (D8), `number->string`
and `string->number` across radixes, and the arithmetic surface. `Rational` and
`Complex` offered with their limits documented.

Leading probe is `7.1`, never `7.0` and never `7`, per CLAUDE.md -- and in this
stage that rule is load-bearing rather than procedural, because the whole stage
is about exact/inexact divergence.

> **What shipped (2026-09-23).**
>
> - **The operators are the lowering's, the tower is the prelude's.** A call
>   `(op a b c)` for `+ - * /` folds onto a binary prelude helper
>   (`r7rs-add2__` ...) that takes a checked path for two exact integers and
>   the promoting dynamic operator otherwise -- which is what makes `(+ 1
>   7.1)` legal (Turmeric's static operator rejects a mixed literal pair,
>   TUR-E0042) and `(+)`, `(*)`, `(- x)`, `(/ x)` mean what R7RS says. A
>   comparison chain `(< a b c)` binds each argument once and tests adjacent
>   pairs. A bare operator in value position names the variadic prelude
>   procedure (`r7rs-+`), so `(map + a b)` type-checks -- but see the
>   deviations. The prelude itself is exempt from the rewrite and from the
>   rename table, since it is written against the typed stdlib's real names
>   and is where the raw operators are allowed.
> - **Exactness is the type** (`exact?` is int, `inexact?` is float),
>   `integer?` accepts 7.0, `exact-integer?` does not; `exact` of an
>   integral float is the int and of 7.1 is an error (no rationals);
>   `inexact`, `exact->inexact`, `inexact->exact`, `nan?`/`infinite?`/
>   `finite?`, `rational?`/`real?`/`complex?` as R7RS-small allows.
> - **D8: checked overflow.** `+`, `-`, `*` and `expt` on exact integers
>   signal on overflow, on both back ends, with one sentence naming R7RS
>   6.2.6 and the deferred bignum epic (`tests/fixtures/r7rs-exact-overflow`,
>   nonzero exit). Until R6's `raise`/`guard` the signal is a panic, not an
>   error object a handler can catch.
> - **Division and rounding.** `(/ 7 2)` is the inexact 3.5 and `(/ 8 2)` the
>   exact 4; `/` by exact zero is an error; `quotient`/`remainder`/`modulo`
>   with R7RS signs, `floor/`, `truncate/` and the four `*-quotient`/
>   `*-remainder` procedures; `floor`/`ceiling`/`truncate`/`round` (ties to
>   even, via a new `rint` in `stdlib/math.tur` next to `trunc`, `tan`,
>   `asin`, `acos`, `atan`); `gcd`/`lcm` variadic; `min`/`max` with inexact
>   contagion; `sqrt` exact for a perfect square, `exact-integer-sqrt`,
>   `expt` exact by checked multiplication for a non-negative exact
>   exponent; `exp`/`log` (one or two arguments)/`sin`/`cos`/`tan`/`asin`/
>   `acos`/`atan` (one or two); `square`, `numerator`/`denominator` on
>   integers.
> - **Number <-> string.** `number->string` and `string->number` take a
>   radix of 2, 8, 10 or 16 (an inexact number is radix 10 only);
>   `string->number` is `#f` on anything that is not a number, through a
>   predicate/accessor pair rather than a sentinel. A float prints as R7RS
>   spells it on both back ends: the shortest of 15/16/17 significant
>   digits that round-trips, `7.0` never `7`, `1e21` not `1e+21`, and
>   `+inf.0`/`-inf.0`/`+nan.0`; the compiled inline-C and the interpreter
>   native share one rule.
> - **Deviations at R5**: no exact rationals (`(/ 1 3)` is 0.3333...,
>   `(exact 7.1)` errors, `numerator` of 7.1 errors); no bignums (overflow
>   signals); no complex numbers (`(sqrt -4)` is +nan.0); `string->number`
>   takes no `#x`/`#e` prefixes; `(apply + xs)` and `(apply max xs)` are the
>   variadic-through-`apply` gap on both back ends
>   (`docs/archive/r7rs-compiled-dynamic-shapes.md` 2b, found here), so
>   `(reduce + ...)`-style code should fold with the binary operator
>   instead; `floor/`, `truncate/` and `exact-integer-sqrt` run under
>   `--interpret` only because their consumer is a two-parameter
>   `call-with-values` (gap 2, R6). `Rational` and `Complex` from the typed
>   stdlib are not reachable from Scheme; the plan's "offered with limits"
>   is the documented absence above.

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

> **What shipped (2026-09-24).** Everything above at the escape level, on
> both back ends, in `stdlib/r7rs/prelude.tur`'s control section and three
> lowerings in `src/compiler/scheme_lower.c`; pinned by
> `tests/fixtures/r7rs-control` (both back ends), `r7rs-uncaught-error`,
> `r7rs-continuation-after-return`, `r7rs-tail-calls` (compiled, `--debug`,
> 1e7 deep for a self call, a mutual pair, an indirect call through a value
> and a named `let`).
>
> - **Tail calls needed no work here** -- D6's prediction held. Every Scheme
>   procedure call is the `EX_DYN_CALL` T6 drives, and the named-`let` shape
>   reaches it once the letrec gap below is closed.
> - **`call/cc` (D7)** is Turmeric's `call/cc` wrapped in `r7rs-call/cc`: the
>   receiver gets a variadic procedure that unwinds the wind stack to the
>   capture point and delivers its arguments as the value (`(k 1 2)` is a
>   `values`). Invoking it after the call/cc has returned is the named error
>   on both back ends. The compiled runtime used to read the prompt's `valid`
>   flag out of the prompt's own dead frame, so the Scheme shape longjmp'd
>   into garbage; it keeps a live-prompt set now (`tur_escape_live_*`,
>   emit_dk_runtime.c / emit_cps_ir.c, the split runtime regenerated), and
>   the interpreter tracks its escape boundaries the same way
>   (`TuriEnv.escape_live`). Re-entry after return stays D7's named gap.
> - **`dynamic-wind`** is a wind stack of before/after pairs; an escape pops
>   the frames above the capture point running each `after`, innermost first.
> - **Exceptions are NOT effects.** D10 named `handle`/`perform`, and a
>   `handle` around a dynamic thunk is not lowered by the compiled back end
>   ("this effect operation has no lowering here"), so the encoding is the
>   one R7RS 6.11 itself describes: a handler stack whose install and
>   uninstall are wind frames, `raise-continuable` calling the innermost
>   handler with the outer handlers installed, `raise` raising a secondary
>   error to the outer handlers when the handler returns, and `guard` as
>   `call/cc` + `with-exception-handler` where both arms hand the
>   continuation a thunk, so the clauses run in the guard's dynamic
>   environment and a clause-less exception is re-raised from there with
>   `raise-continuable` (the standard asks for the raise's environment,
>   which needs re-entry). An uncaught `raise` reports on stderr after
>   flushing stdout and exits 70. Error objects are `R7rsError`
>   (`error`, `error-object?`, `-message`, `-irritants`; `read-error?` and
>   `file-error?` answer `#f` until R8).
> - **`parameterize` is not `dynvar`.** `defdynamic` declares a static
>   variable; `make-parameter` makes one at run time, so a parameter is a
>   closure over a heap cell (`R7rsParam`) that `parameterize` reaches
>   through a private marker argument, converts the new value through the
>   parameter's converter, and swaps in and out under `dynamic-wind`.
> - **Promises** are the R7RS 7.3 reference shape: a promise shares a box
>   (`R7rsPBox`), `delay` is `delay-force` of `make-promise`, and `force`'s
>   self call is a tail call, so a 100,000-deep `delay-force` chain runs in
>   constant space.
> - **The compiled gaps closed** (docs/archive/r7rs-compiled-dynamic-shapes.md,
>   the four fixtures that carried `requires.interp-only` run compiled):
>   the letrec placeholder in a dynamic file is `any` and a lambda init is
>   pinned to match; every Scheme lambda returns `any`; a fn type's box id
>   spells its rest slot, the emitter registers each boxed variadic with its
>   fixed count, and a dynamic call whose id compare fails packs the surplus
>   arguments into the `(Cons any)` chain (`__tur_dyn_call_var`, and the T6
>   trampoline's `__tur_tb_invoke`), the fat shim typing the rest slot as
>   the chain pointer; a capturing closure's value type carries the rest
>   marker; the H8 adaptor declines a variadic; the interpreter packs at its
>   dynamic call. Also found and fixed on the way: a closure that `set!`s a
>   mutable global read it before its declaration (the global forward-decl
>   pass did not descend the `any` widen), a `case-lambda` whose no-match arm
>   was a bare `panic` lost its outer arms' results, and a top-level
>   `(define f (lambda ...))` is a `defn` now, so it keeps its variadic
>   signature and forward-references.
> - **Still capped:** a dynamic call with more than four arguments
>   (`emit_dyn_call`, so `apply` too); the `TUR_APPLYn_T` table stops at 4.
> - **Not this stage's:** `list-length` on a `(Cons any)` chain
>   (docs/reported/list-length-on-cons-any-segfaults.md, which is what the
>   archived report's section 3 really was).

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

A prerequisite the R3 seam found: a Turmeric generic constructor called with
nothing to bind its type parameters (`(map-new)`, and any adaptor that
forwards to one) hands Scheme an OPEN type that nothing grounds to `any`, so
the first insert is a static error or a compiled-only panic
(`docs/reported/saffron-open-generic-result-not-grounded.md`). Every
adaptor here that forwards to such a constructor either ascribes the result
itself (`(:: (map-new) (Map any any))`) or waits on that report; the D9
snippet as written (`(hamt-set (hamt-new) "k" 42)`) is the second face of it.

> **What shipped (2026-09-24).** Every R7RS-small procedure that is not a
> port, on both back ends, with the libraries split by cost:
>
> - **Resident** -- `base`, `case-lambda`, `char`, `complex`, `cxr`,
>   `inexact`, `lazy`, `write` -- live in `stdlib/r7rs/prelude.tur`, so
>   importing one is a scoping statement. New in `(scheme base)`: variadic
>   char and string comparisons, `boolean=?`, `symbol=?`, `list-set!`,
>   `make-list`, `make-string`, `string`, the optional `[start [end]]` ranges,
>   multi-list `map`/`for-each`, `string-map`/`string-for-each`,
>   `vector-map`/`vector-for-each`/`vector-append`/`vector-copy`/
>   `vector-copy!` (overlap-safe), the bytevector copies and appends,
>   `string->utf8`/`utf8->string` (decoding checked), `member`/`assoc` with a
>   comparison, `apply` with leading arguments, `rationalize`, `features`,
>   `write-simple`. `(scheme char)` case mapping is ASCII (the typed stdlib
>   has no Unicode tables; a non-ASCII char maps to itself). `(scheme
>   complex)` answers for reals and refuses a complex result.
> - **On demand** -- `time`, `process-context`, and `file`'s
>   `file-exists?`/`delete-file` -- are files under `stdlib/r7rs/` that the
>   load expander splices in when a Scheme file imports them
>   (`scheme_import_library_files`), so an unimporting program carries none
>   of it and the names stay free for its own use. `exit` runs the
>   outstanding `dynamic-wind` afters; `delete-file` raises an error object
>   `file-error?` recognizes (error objects gained a kind).
> - **Deferred, with the reason at the import** -- `eval`, `repl`, `load`
>   (Section 8, question 3: decided, they wait for an evaluator at run time)
>   and `read` (R8). `include`/`include-ci` are refused at the form (an
>   included file would have to be read as Scheme without a `#lang` line), as
>   are `string-set!`/`string-fill!`/`string-copy!` (strings are immutable
>   `cstr`). An unknown `(scheme ...)` name is an error.
>
> **Adaptors, not reimplementations, with one measured exception**: the
> on-demand libraries carry their own inline-C primitives (with interpreter
> twins) instead of forwarding to `fs`/`env`/`process`/`time`, because those
> typed functions are effect-annotated inline C with no interpreter natives
> -- forwarding would have made the libraries compiled-only -- and `time`
> has no sub-second wall clock or monotonic counter at all.
>
> Found and fixed on the way, most of them outside Scheme:
>
> - A static call to a variadic did not narrow an `any` argument into a
>   concrete FIXED parameter (only the widening direction existed), so
>   `(string->list s)` with `s : any` was a cc error compiled and garbage
>   interpreted (`saffron-variadic-fixed-arg-narrow`).
> - A typed variadic passed as a value was boxed as itself and called through
>   the all-`any` protocol -- `(member x l =)` answered wrong. It now gets an
>   all-`any` variadic adaptor that forwards its rest list (a `__rest-chain`
>   marker the variadic call path honours), and only all-`any` variadics are
>   registered for packing.
> - A variadic's forward declaration counted `&` as a parameter and had no
>   rest marker, and an annotated `: any` result forward-declared as `int`
>   (`saffron-forward-ref-any-and-variadic`) -- which retires R5's "a
>   variadic must be defined before its callers" rule.
> - Import and load file ids started at a fixed 10 while the compiled driver
>   numbers its ~40 auto-loaded files from 1, so any `(load ...)` overwrote
>   an auto-loaded file's source record (here, `typeclass-alternative.tur`
>   became a Scheme file). The driver now records where its band ends and
>   ids start there; the registry that holds them grew from a hard-coded 64
>   to `DIAG_MAX_FILES` (512), since a procedural macro's compile-time
>   evaluation alone used the ~20 ids left above the band.
> - Float literals were emitted with `%.15g`, so `3.141592653589793` compiled
>   as a different double (`float-literal-round-trip`).
> - A vector literal is self-evaluating and built by `vector` (the reader
>   stamps `#(` vectors, PROV_SCHEME_VECTOR), so `#()` is a `(Vec any)`.
>
> Deviations: ports and everything built on them are R8; `command-line`'s
> first element is `"tur"` (`*args*` does not carry argv[0]); a program that
> imports nothing still sees every resident name.
>
> R8 update: `(scheme read)` is now on demand (`stdlib/r7rs/read.tur`), and
> `(scheme file)` gained its port half.
### R8 -- ports and I/O (medium)

The port taxonomy, string ports, `read`, `write`, `display`, `write-shared` and
`write-simple` (which need cycle detection, so they pair with R3's `equal?`).

> **What shipped (2026-09-24).** All of it, on both back ends
> (`tests/fixtures/r7rs-ports`, `r7rs-write-labels`, `r7rs-read`,
> `r7rs-file-ports`).
>
> - **Ports** are an `R7rsPort` record (kind, direction, binary, open, the
>   `#!fold-case` state) over an `R7rsIo`: one C byte buffer with a read
>   position and, for a file port, its `FILE`. A string or bytevector port is
>   the buffer alone; an input file port fills the buffer on demand, so
>   peeking a multi-byte character needs no `ungetc`; an output file port
>   writes straight through, flushing stdout before anything reaches stderr.
>   The primitives are inline C with interpreter twins, and both handles are
>   `defopaque ... :ptr<void>`, not `:int`. D3's table said a port is a
>   `defopaque` over the existing stream layer; it is a record over an opaque
>   buffer instead, because a port carries state of its own (direction, open,
>   fold-case), and `stdlib/io.tur`'s stream functions have no interpreter
>   natives, so forwarding to them would have made ports compiled-only.
> - **The I/O surface of `(scheme base)`**: `read-char`/`peek-char`/
>   `read-line` (LF, CR or CR LF)/`read-string`/`read-u8`/`peek-u8`/
>   `read-bytevector`/`read-bytevector!`/`char-ready?`/`u8-ready?`,
>   `write-char`/`write-string`/`write-u8`/`write-bytevector` with their
>   ranges, `newline`, `flush-output-port`, the string and bytevector port
>   constructors and getters, `close-port` and its two directions,
>   `call-with-port`, and the seven predicates. Text is UTF-8: a character is
>   decoded on read and encoded on write. Using a closed port, or one of the
>   wrong direction, raises an error object.
> - **The current ports are parameter objects**, so `parameterize` rebinds
>   them with R6's machinery, and the uncaught-exception report goes to
>   `(current-error-port)`.
> - **The printer.** `write` and `display` label only the pairs and vectors
>   on a cycle (`#0=`/`#0#`), found by a pre-pass that walks list spines
>   iteratively and keeps its state in an identity hash table; `write-shared`
>   labels everything that appears twice; `write-simple` labels nothing.
>   `write` escapes strings (`\t`, `\n`, `\xHH;` ...), names characters
>   (`#\null` ... `#\delete`, `#\xHH` for other control characters) and bars
>   a symbol that would not read back (`|two words|`, `||`, `|12|`).
> - **`(scheme read)`** is an on-demand library (`stdlib/r7rs/read.tur`)
>   reading the R7RS external representation from a port: lists, dotted pairs
>   and brackets, vectors, bytevectors, strings with every escape and the line
>   continuation, `|...|` symbols, characters by glyph, name or hex, the
>   boolean spellings, numbers with their prefixes, the quote abbreviations,
>   all three comment forms, `#!fold-case` (kept per port), and datum labels
>   including cycles (placeholders patched after the labelled datum is read).
>   A malformed datum raises an error object `read-error?` recognizes.
> - **`(scheme file)`'s port half**: the four `open-*-file` procedures (a
>   failed open raises a `file-error?` object naming the path), the two
>   `call-with-*-file` procedures, and `with-input-from-file`/
>   `with-output-to-file` over `parameterize`.
>
> Found and fixed on the way:
>
> - A record field typed as a pointer opaque was laid out as the int64 word
>   while the constructor's argument was spelled `void *` (an int-conversion
>   warning, an error on newer compilers), and `set!` of a field of such a
>   record, whose parameter arrives as the carrier word, emitted
>   `((int64_t *)p)->field`. The constructor call relabels the argument and
>   the store casts to the record's own cell, as the field read already did.
> - A vector was not `eq?` to itself under the interpreter: a widen to `any`
>   wraps a vector in a fresh box each time, and identity compared the boxes.
>   The identity natives now look through the box.
> - `eqv?` on two records (ports, `define-record-type` values, procedures)
>   fell through to the numeric `=` and panicked; it is identity now.
> - Binding the value of a `nil`-returning call (`(let ((r (display x)))
>   ...)`, and every `guard` whose body ends in one) was TUR-E0023 in a
>   dynamic file. It binds the unspecified value now.
> - `tests/run-r7rs-import.sh` (not part of `run.sh`) had been red on its
>   compiled cases since R6/R7: a program that imports a module compiles the
>   prelude as a module, and the `call/cc` presence scan did not look inside
>   module bodies, so the escape runtime was never emitted; and
>   `r7rs-apply-list__`, called before its definition with no result
>   annotation, forward-declared as `int`. The scan descends modules (as the
>   serial-runtime scan already did) and the result is annotated `: any`.
>
> Found and filed, not fixed: on the compiled path a top-level `define`'s
> initializer runs before every top-level expression, in every dialect
> ([toplevel-def-initializers-run-before-toplevel-expressions](../reported/toplevel-def-initializers-run-before-toplevel-expressions.md)).
> It matters once initializers have effects -- `(define p
> (open-output-file ...))` opens the file before an earlier top-level write.
>
> Deviations: `char-ready?`/`u8-ready?` always answer `#t`; a port is one
> direction, never both; `#e1.5` is an error (no exact rationals, D8).

### R9 -- tooling (medium; parallelizable)

`tur repl --lang r7rs`, `tur fmt` preserving `#lang r7rs` and formatting Scheme,
`tur init --r7rs`, the LSP (which 2.6 suggests works by inheritance -- **measure
it, do not assume it**, since that is exactly how `tur fmt` and module import
were both caught), editor packs, `tools/gendocs.py`, and
`docs/guides/r7rs-guide.md`.

> **What shipped (2026-09-24).** Every item, each MEASURED first -- and the
> measuring earned its keep: of the seven items, one worked by inheritance
> (LSP diagnostics), and the other six were broken or missing, four of them in
> code paths no fixture reaches.
>
> - **`tur repl --lang r7rs`** (`tests/turi/repl-lang-r7rs.sh`, ctest
>   `tur_repl_lang_r7rs`). `--lang` was a hard-coded turmeric/saffron pair; it
>   now takes any base `tur dialects` lists. `#lang r7rs` at the prompt was
>   worse than refused: it switched the language and kept Turmeric's pinned
>   preload, and the prompt's `<eval>` text was exempt from the Scheme renames
>   (R7's exemption for synthetic sources), so `(display ...)` met Turmeric's
>   `display` method. A dialect whose prelude differs now gets a fresh session
>   (`repl_fresh_env`, also behind `:reset` and `:run`, which keep the
>   dialect), and the exemption stops at the pinned preload's last line
>   (`g_synthetic_user_from_line`). An R7RS session echoes through the
>   prelude's `write` (`=> (a "b" #\c)`, not `#<struct Sym>`). The browser
>   REPL's language picker had the same gap and the same fix
>   (`tur_wasm_glue_lang_unit`).
> - **`tur fmt`** (`tests/run-fmt.sh`, five `fmt-r7rs-*` cases). The form
>   printer cannot format Scheme: the reader desugars `#\x`, `#u8(...)`,
>   `#e1.5e2` and `|two words|`, so printing the forms back made a different
>   program (measured: all of those, `#t` as `true`, `,` as `~`). A Scheme file
>   is RE-INDENTED -- every token and line break kept, each line's leading
>   whitespace recomputed by Lisp rules (body forms two past the opener, calls
>   aligned under the first argument, data under the first element), lines
>   inside strings, `|...|`, `#| |#` and inline C untouched. `--stdin --lang
>   r7rs` for an editor selection. Found on the way: `fmt-bootstrap-stdlib` had
>   been red since R2 -- every `stdlib/r7rs/` file went through the Turmeric
>   printer, and `run-fmt.sh` is not part of `run.sh`. The r7rs stdlib is now
>   self-formatted and the harness is 34/34.
> - **`tur init --r7rs`** (`tests/run-init-r7rs.sh`, ctest `tur_init_r7rs`): a
>   binary that is a top-level Scheme program, a `--lib` that is a
>   `define-library`, each with a test `tur test` runs. The first draft found
>   three defects, all in project mode (`tur build .`), which no fixture uses:
>   the bin/lib decision is a text scan for `(defn main`, so a Scheme program
>   built a shared library (a `#lang r7rs` file without a `define-library` is
>   now an entry point); a library was refused because the prelude -- itself a
>   `#lang r7rs` file in the same form stream -- counted as "something else at
>   top level" (prelude forms are now lowered in place, outside the user's
>   library or program module); and the per-module C emission never had the
>   forward-declaration band for globals that single-file emission has, so the
>   prelude's handler stack was used before its declaration.
> - **The LSP** (`tests/lsp/r7rs-diagnostics.py`, ctest
>   `lsp_r7rs_diagnostics`; `tur_lsp_wasm_backend_unit`). Diagnostics work by
>   inheritance, as for Saffron, and are now pinned (with the broken-buffer
>   control first). Formatting did NOT work for any `#lang` document:
>   `textDocument/formatting` handed the directive to the reader and answered
>   "no edits". The `#lang`-aware document formatter moved from `main.c` into
>   `fmt.c` (`fmt_format_document`) so the LSP runs the same one. The browser
>   LSP -- the R0 sweep's flagged site -- now detects the dialect, sets the
>   file's language and selects its prelude per buffer.
> - **Editor packs** (`tests/run-editor-syntax.sh`): the Scheme lexemes
>   (`#t`, `#\x`, `|sym|`, `#| |#`, `#;`, radix numbers, `#(`/`#u8(`, the
>   Scheme forms), scoped to `#lang r7rs` files in both packs -- a `|...|`
>   symbol rule would otherwise swallow the `|` of Turmeric's
>   `#refine{x : T | pred}`, which the harness now checks.
> - **`tools/gendocs.py`** (`tests/check-gendocs-parse.sh`): measured to find
>   a Scheme library's exports and NO definitions, under the file's name
>   rather than the library's. It now reads `define`, `define-syntax` and
>   `define-record-type`, and names the module from `define-library`.
> - **`docs/guides/r7rs-guide.md`**, its examples compiled and run on both
>   back ends by `tests/fixtures/docs-r7rs-guide-examples`. Drafting it
>   corrected two claims before they shipped: `(except ...)` imports are
>   refused (R3), and exact overflow is a panic, not a condition `guard` can
>   catch.
>
> Not verified here: the browser builds of the REPL and LSP changes (no
> Emscripten in this environment). Both are exercised natively by their unit
> tests, which fail without the fixes.

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

1. **Multiple values.** ~~Turmeric has no `values`/`call-with-values`.~~
   **Decided at R2**: a `Values` carrier over a list, with `(values x)` being
   `x` itself so a single value never allocates; `let-values`,
   `let*-values` and `define-values` desugar to indexed reads of the carrier
   and `call-with-values` goes through `apply`. The ABI convention was not
   needed and stays available if R6's profiling wants it.
2. **Static-datum mutation detection** (D4). How does `set-car!` cheaply refuse
   a `.rodata` pair -- an address-range test, a spare tag bit, or a
   debug-build-only check? R7RS says "it is an error", which permits undefined
   behavior, but silently corrupting `.rodata` is a bad answer even when the
   standard allows it.
3. **`(scheme eval)` in compiled programs.** Link `libturi` into every emitted
   Scheme program, link it on demand when `(scheme eval)` is imported, or
   declare `eval` interpreter-only? The third is honest and cheap; the second is
   the right answer and needs a mechanism. **Decided at R7: neither yet.**
   `(scheme eval)`, `(scheme repl)` and `(scheme load)` are refused at the
   import with this question named. Even under `--interpret`, `eval` needs a
   datum-to-Form path and the lowering at run time, which is its own piece of
   work; the on-demand library mechanism R7 built is where a linked evaluator
   would plug in.
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
