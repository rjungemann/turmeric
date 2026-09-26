# SRFI libraries for `#lang r7rs`, after Racket's

Status: **plan, nothing landed.** Every "today" claim below was measured on
2026-09-26 against `./build/tur` at fdd51fc9 (Debug build), on both back ends
(`tur run` and `tur --interpret`). The transcript is in
[Appendix A](#appendix-a----probe-transcript). Every claim about Racket was
read from Racket's own sources on the same day ([Appendix B](#appendix-b----racket-evidence)).

---

## 0. The ask

> SRFI support for R7RS similar to Racket's. Some features will already be
> implemented, and some won't make sense to implement, and that's okay. Making
> already-implemented ones a no-op when importing (if Racket does same, for a
> given SRFI) would be useful, for new people to Turmeric that are trying out
> R7RS. Also a support table in the R7RS guide would be good.

That is three deliverables, and they are staged in that order of value to a
newcomer:

1. **`(import (srfi N))` works wherever Racket's `(require srfi/N)` works for
   an SRFI the R7RS core already covers**, and costs nothing. That covers most
   of what someone pasting a snippet from a Scheme tutorial will write. It is
   S1, and it is small.
2. **A support table in `docs/guides/r7rs-guide.md`**, so the answer to "does
   Turmeric have SRFI N?" is one lookup. It also lands in S1, because the table
   is only honest once imports resolve.
3. **Real implementations of the SRFIs people use**, in order of demand:
   syntax sugar (S2), lists (S3), hash tables (S4), strings (S5), formatting
   and testing (S6), then the rest of Racket's list (S7, S8).

---

## 1. What Racket does (read from source)

**The import.** Racket's `#lang r7rs` (the `r7rs` package) maps a library name
to a module path by joining its parts with `/`. Integers are allowed as parts,
as R7RS 7.1.7 says. So `(import (srfi 1))` *is* `(require srfi/1)`: Racket's
R7RS SRFI support is simply its `srfi` collection, one module per SRFI.

**The collection.** Racket's SRFI manual documents 49 SRFIs: 1, 2, 4, 5, 6, 7,
8, 9, 11, 13, 14, 16, 17, 19, 23, 25, 26, 27, 28, 29, 30, 31, 34, 35, 38, 39,
40, 41, 42, 43, 45, 48, 54, 57, 59, 60, 61, 62, 63, 64, 66, 67, 69, 71, 74,
78, 86, 87 and 98. That list is this plan's scope (Section 5 has the rest).

**The SRFIs Racket's core already covers** come in exactly three shapes:

| Shape | SRFIs | What `(require srfi/N)` does |
|---|---|---|
| Re-export | 6, 16, 23, 28; 39 (with a guard tweak) | `srfi/6.rkt` is `(provide get-output-string open-input-string open-output-string)`: the core bindings, renamed nowhere. Importing it changes nothing. |
| Empty module | 30 | `srfi/30.rkt` is `;; Supported by core PLT, nothing to provide`. The import succeeds and binds nothing. |
| No module | 62 | The manual says "This SRFI's syntax is part of Racket's default reader (no `require` is needed)", and there is no `srfi/62.rkt`. The import is an error. |

Everything else is its own library. That includes 9, 11, 34, 87 and 98, which
racket/base does not match exactly: racket/base's `let-values` has no dotted
rest, its `case` has no `=>`, and so on. R7RS adopted those SRFIs, so here they
belong in the first two shapes (Section 3, D2).

Racket ships no module for SRFI 0 (`cond-expand`), 46 (custom ellipsis) or 105
(curly-infix), and none for the newer R7RS-era SRFIs (111, 113, 125, 128, 133,
141, 151, 158); those come from third-party packages.

---

## 2. What Turmeric has today (measured)

### 2.1 No `(srfi N)` import resolves

```
$ tur run srfi1.tur        # (import (scheme base) (scheme write) (srfi 1))
srfi1.tur:2:44: error: a library name part must be an identifier
```

`library_module` (src/compiler/scheme_lower.c) takes only identifiers as
library-name parts, so the `1` is refused before any lookup happens. The same
happens under `--interpret`.

### 2.2 The R7RS core already is most of the "core" SRFIs

Probed on both back ends, all correct:

| SRFI | Probe | Result |
|---|---|---|
| 0 | `(cond-expand ((library (scheme base)) ...))` | taken |
| 6 | `write` into an `open-output-string`, `get-output-string`, `read-char` from an `open-input-string` | `("hi" #\z)` |
| 9 | `define-record-type` with a modifier | `(#t 5 2)` |
| 11 | `(let-values (((a . rest) (values 1 2 3))) ...)` | `(1 (2 3))`, dotted rest included |
| 16 | `case-lambda` | works |
| 23 | `(error "bad" 1 2)` under `guard` | `("bad" (1 2))` from `error-object-message` and `error-object-irritants` |
| 30 | `#\| nested #\| comment \|# \|#` | skipped |
| 34 | `(guard (e ((assq 'a e) => cdr)) (raise ...))` | `42` |
| 38 | `write-shared` on a cycle; `read` of `"#0=(a b . #0#)"` | `#0=(1 2 . #0#)`; `(a #t)`, the cycle rebuilt |
| 39 | `(make-parameter 10 (lambda (x) (* x 2)))`, then `parameterize` to 3 | `(20 6)`: the converter runs on the initial value and on each `parameterize`, as SRFI 39 says |
| 45 | `(force (delay-force (delay 7)))`, `(force (make-promise 8))` | `7`, `8` |
| 46 | `(syntax-rules ooo () ((_ x ooo) (list x ooo)))` | works, but not with `:::` (2.3) |
| 62 | `(list 1 #;(ignored) 2)` | `(1 2)` |
| 87 | `(case 5 ((5) => (lambda (x) (* x 10))) (else 0))` | `50` |
| 98 | `get-environment-variable` under `(scheme process-context)` | works |
| 105 | `{1 + 2}` in a `#lang r7rs` file | `3` |

### 2.3 Three defects the probes found (filed)

- **An identifier that starts with `:` does not read as a symbol in source.**
  `':x` reads as the symbol `x`, and `':::` and a parameter named `:x` are
  errors, while `read` gets all three right. This blocks SRFI 42, whose
  generators are all named `:list`, `:range` and so on, and the `:::`
  ellipsis. A colon *inside* an identifier is fine, so SRFI 14's
  `char-set:letter` is not affected.
  [docs/archive/r7rs-leading-colon-identifiers.md](../archive/r7rs-leading-colon-identifiers.md)
  (resolved: fixture `r7rs-colon-identifiers`; the wider question it raised,
  Turmeric surface in Scheme source, is
  [docs/reported/r7rs-turmeric-syntax-leaks.md](../reported/r7rs-turmeric-syntax-leaks.md))
- **A `define-library` cannot export a `syntax-rules` macro.** The export check
  in src/compiler/elab_module.c refuses it: "exported symbol 'my-rec' is not
  defined in this module". That rules out the obvious design of SRFI
  libraries as ordinary library modules (D3).
  [docs/reported/r7rs-define-library-cannot-export-syntax.md](../reported/r7rs-define-library-cannot-export-syntax.md)
- **`(features)` lists `ratios`, but `cond-expand` says it is absent.** They
  are two hand-kept copies of one list. S1 adds a `srfi-N` identifier per SRFI,
  so it generates both from one table (D4).
  [docs/archive/r7rs-cond-expand-ratios-feature-drift.md](../archive/r7rs-cond-expand-ratios-feature-drift.md) (resolved: fixture `r7rs-features-agree`)

### 2.4 What the design can lean on

- **A spliced Scheme file carries its macros to the file that splices it,
  hygienically.** A `(load "util.scm")` whose file defines a `syntax-rules`
  macro and a helper works on both back ends. Referential transparency holds:
  the template's helper keeps its meaning under a use-site `let` that rebinds
  the same name. It also works when a user library `include`s the same file
  the program loads.
- **On-demand library splicing already exists.** `(scheme time)`,
  `(scheme file)` and the others are `stdlib/r7rs/*.tur` files the load
  expander splices in only when a Scheme file imports them
  (`scheme_import_library_files`, `SCHEME_LIBS[]`, `ONDEMAND[]`). Their names
  are renamed onto the library's spelling only once imported, so a program
  that does not import `(scheme time)` can define its own `current-second`.
  A program and a user library that both import one are deduplicated by the
  load expander's visited set; probed on both back ends.
- **Only inline C costs double.** Every inline-C body in an on-demand library
  has an interpreter twin in src/turi/interpreter_natives.c ("Keep them
  equal"). A library written in Scheme needs no twin.

### 2.5 Representation facts the implementations must respect

- `string-ref` on an immutable string (every literal) scans the UTF-8 from the
  start (`r7rs-utf8-ref__`). A mutable string is a code-point vector and
  indexes in constant time. So an index-based string SRFI (13) written
  naively over `string-ref` is quadratic on literals.
- Bignums are base-1e9 limbs (stdlib/r7rs/bignum.tur), not binary. Bitwise
  SRFIs (60) need a conversion or an arithmetic formulation.
- `stdlib/random.tur` is libc `rand()` seeded from the clock, with no state a
  program can save and restore. SRFI 27 needs exactly that.
- Scheme values kept in libc-allocated memory are invisible to the collector
  (r7rs-guide, Memory). An SRFI data structure is built from Scheme values
  (vectors, pairs, records), or its C storage is rooted and region-noted per
  CLAUDE.md's region store rule.

---

## 3. Design decisions

### D1 -- `(srfi N)` is a built-in library family, looked up in a table

`(srfi N)` is resolved like `(scheme base)`, by a table in the lowering
(`SRFI_LIBS[]`, beside `SCHEME_LIBS[]`), not by the module loader. The module
route (`srfi/1` as a Turmeric module) would inherit every restriction of a
user library: no exported syntax (2.3), `except` that hides nothing (guide,
"Where it differs"), exports typed `any` on the Turmeric side, and a separate
compile per library. The table route inherits the `(scheme ...)` libraries'
behaviour, which is already right.

The accepted spelling is exactly `(srfi N)`, with `N` an exact non-negative
integer, as in Racket's R7RS. An unknown `N` is an error that names the
guide's table. SRFI 97's `(srfi :1)` and `(srfi 1 lists)` spellings are not
accepted (Section 7, question 2).

Integer library-name parts are accepted for `(srfi N)` in S1. For a user
library (`(mylib 2)`, which R7RS also allows), S1 checks whether `mylib/2`
survives module-path resolution and C name mangling. If it does, it is
accepted the same way; if not, the error says so.

### D2 -- an import succeeds exactly where Racket's does; it costs nothing where the core already is the SRFI

Every table row has one of five kinds:

| Kind | Import does | Rows |
|---|---|---|
| **built in** | Binds the SRFI's names to the existing `(scheme ...)` bindings. No code is spliced or emitted. `only`/`except`/`prefix`/`rename` apply to the SRFI's export list, and importing it next to `(scheme base)` is never a conflict: it is the same binding. | 6, 9, 11, 16, 23, 30 (empty export list), 34, 39, 87, 98 |
| **alias** | Built-in bindings under the SRFI's spellings, plus the odd one-liner. | 38, 45 (and 66, S7) |
| **library** | Splices the SRFI's implementation in (D3). | the rest, by stage |
| **no library** | An error that says the syntax is always on and the import can be deleted. | 62, as in Racket |
| **not supported** | An error with the reason, and the guide's table. | 40 (deprecated in favour of 41) and every row not landed yet |

Where Racket has its own module but R7RS adopted the SRFI (9, 11, 34, 87, 98),
the row is **built in** here. A newcomer sees the same thing in both systems:
the import works and the names mean what the SRFI says. Here it also costs
nothing, because R7RS's own forms already *are* the SRFI's. Racket needs a
separate module only because racket/base's forms differ.

**SRFI 62 follows Racket and is not importable.** The ask says to make the
import a no-op only where Racket does, and Racket has no `srfi/62`. The error
is kinder than a missing module, though: "SRFI 62's `#;` comments are part of
the reader and always on; this import can be deleted." Flipping it to a no-op
later is one table cell (Section 7, question 1). SRFI 0, 46 and 105 have no
Racket module either; the table lists them as built in, with nothing to
import.

### D3 -- an SRFI is a `define-library` file spliced into its importer, not a module

Each supported SRFI is one file, `stdlib/r7rs/srfi/<N>.tur`, holding one
`(define-library (srfi N) ...)` written in plain R7RS. That mirrors Racket's
`srfi/<N>.rkt`: one file per SRFI, and a built-in row's file is only an
export list, as `srfi/6.rkt` is. Importing `(srfi N)` splices the file in
through the load expander, as `(scheme time)` is spliced today
(`scheme_import_library_files` consults `SRFI_LIBS[]` too). The lowering then
takes the `define-library` in **inline mode**:

- Each body definition is spelled `srfi<N>--<name>`, so it can collide neither
  with a user's definition nor with an auto-loaded Turmeric stdlib name
  (`filter`, `fold`, ...). The export list becomes rename rows,
  `<public name> -> srfi<N>--<name>`, which apply only in a file that
  imports the SRFI, exactly like `ONDEMAND[]` rows.
- `define-syntax` in the body registers the macro in every lowering pass that
  imports the SRFI. That matters because a macro emits no code: splicing the
  definitions once per compile (the visited set) is right for procedures and
  wrong for macros. Templates refer to the respelled private helpers, which
  are global, so referential transparency holds, as it does for `load` today
  (2.4).
- A built-in row's body is empty, and its exports are re-exports of
  `(scheme ...)` names, so inline mode emits nothing. That is what makes the
  no-op literal (S1's exit criterion).
- The library's own `(import ...)` declarations splice its dependencies
  first. `lib_files_of_set` already walks a `define-library`'s imports, so
  `(srfi 13)` pulls in `(srfi 14)`, and `(srfi 98)` pulls in
  `(scheme process-context)`.

This sidesteps the macro-export gap (2.3) rather than waiting for it, and it
lets SRFI reference implementations port with small edits, checkable against
chibi or Racket. Fixing the gap for user libraries stays its own report.

**Extensions of a core form are lowering arms, not macros.** SRFI 61 (a
`cond` clause), 17 (`set!` on a call), 5 and 71 (`let`) change what an
existing core form accepts. A `syntax-rules` macro cannot extend `cond`
without replacing it. These get an arm in the core form's lowering, gated on
`srfi_imported[N]`, so a program that does not import the SRFI keeps strict
R7RS behaviour.

**Scheme first, C only for primitives.** An inline-C body costs an
interpreter twin (2.4), a region-note audit and a GC-rooting audit (2.5).
Only a real primitive gets one: the hash functions (S4) and the clock and
calendar calls (SRFI 19).

### D4 -- one table drives import, `cond-expand`, `(features)` and the guide

`SRFI_LIBS[]` carries each row's number, kind, title, file, the reason for
refusal (if any), and the stage that lands it. From it:

- the import (D1, D2);
- `cond-expand`'s `(library (srfi N))`, which holds for built in, alias and
  library rows;
- a `srfi-N` feature identifier per importable row, the naming convention
  SRFI 0 introduced. It goes into `feature_holds` and into `(features)`'s list, both
  generated from the table, which also fixes the `ratios` drift (2.3);
- the guide's support table, which a sync check compares against the table.

`tests/check-r7rs-srfi-sync.sh`, alongside the existing
`check-r7rs-unicode-sync.sh` and `check-r7rs-inc-sync.sh`, fails when:

- a row names a file that does not exist, or a file has no row;
- a row's kind disagrees with its file (a built-in file with a body);
- the guide's table disagrees with the rows.

### D5 -- a conflicting name is an error at the import, with the fix in it

R7RS 5.2 makes importing one identifier with two different bindings an error.
Most SRFI names that `(scheme base)` also has are **compatible extensions**,
and those bind to the base procedure. That covers SRFI 1's `map`,
`for-each`, `member`, `assoc`, `list-copy` and `append`, and SRFI 13's
`string-copy`, `string-copy!` and `string-fill!` with their optional
start/end. Importing both is then the same binding, not a conflict. Racket's
`srfi/1` defines its own `map`; this is deliberately kinder.

A few names are **incompatible**:

- SRFI 43's `vector-map` and `vector-for-each` pass the index first.
- SRFI 13's `string-map` and `string-for-each` take one string plus
  start/end, where R7RS takes several strings.

Importing an incompatible name alongside `(scheme base)` is an error that
names both libraries and spells out the fix, e.g.
`(import (except (scheme base) vector-map vector-for-each) (srfi 43))` or
`(prefix (srfi 43) v:)`. Importing the SRFI without `(scheme base)` needs no
fix: `(scheme base)` names are global either way, and the explicit import
decides.

### D6 -- where an implementation comes from

- **Port the reference implementation** when it is portable R5RS/R7RS under an
  MIT-style licence, which covers most of Racket's list. Keep its copyright
  header, with a licence note in `stdlib/r7rs/srfi/COPYING` as
  `tests/r7rs/CHIBI-COPYING` does for chibi.
- **Write our own** when the reference is tied to one implementation's
  primitives (SLIB) or is slow over our representation. That covers the
  string SRFIs, which work over a code-point vector rather than repeated
  `string-ref` (2.5), and the hash tables (S4).
- **Adapt a typed stdlib operation** where one fits exactly, as the prelude
  does ("adaptors, not reimplementations"). That covers the fixnum fast path
  of SRFI 60 over Turmeric's bit operations. It does not cover SRFI 27, whose
  state requirements `stdlib/random.tur` cannot meet.

### D7 -- tests: a fixture per SRFI, and the SRFI's own suite where one exists

- `tests/fixtures/r7rs-srfi-<N>` for every row that lands, run on both back
  ends (`tests/run.sh` compiled, `tests/run-turi.sh` interpreted).
- An SRFI's own test suite, where the SRFI document or chibi-scheme
  (`lib/srfi/<N>/test.sld`, BSD, the licence already vendored) ships one, goes
  under `tests/r7rs/srfi/<N>/` and runs through the conformance runner
  (`tests/r7rs/run-conformance.py`, which already speaks chibi's test
  vocabulary). It reports a count per SRFI with a floor, as
  `tur_r7rs_conformance` does. S0 inventories which suites exist.
- Negative fixtures under `tests/fixtures/errors/` for each refusal message
  and each conflict.

### D8 -- no new `--enable`: the `r7rs` experiment already covers it

CLAUDE.md puts in-flight compiler features behind `--enable=<name>`.
`#lang r7rs` is itself that gate: its directive enables the `r7rs`
`EXPERIMENTS[]` row, and TUR-W0060 fires on every compile. SRFI imports are
additive surface inside the dialect, so a program that imports no SRFI sees no
change. A nested `--enable=r7rs-srfi` would make every newcomer pass a flag to
use `receive`, which defeats the point. The `r7rs` row's `plan_path` stays on
r7rs-lang-plan.md, which links here from its See also section.

---

## 4. The support table (target)

This is the table the guide gets in S1. The "Here" column is the **target**.
When S1 lands, a row whose stage has not landed yet reads **not yet** and its
import is the "not supported" error. Each later stage flips its rows.

Legend:

- **built in**: importing is a no-op; the R7RS core already is the SRFI.
- **alias**: a few new names for built-in procedures.
- **library**: importing loads the implementation.
- **no library**: the syntax is always on and there is nothing to import.
- **not planned**: the import is refused, with the reason.

| SRFI | Title | Racket | Here | Stage | Notes |
|---|---|---|---|---|---|
| 0 | Feature-based conditional expansion | no module | built in, no library | -- | `cond-expand` is R7RS; `srfi-N` feature ids are added (D4) |
| 1 | List Library | library | library | S3 | base-compatible names are the base bindings (D5); the linear-update `!` procedures are the pure ones, which the SRFI allows |
| 2 | AND-LET* | library | library | S2 | `syntax-rules` |
| 4 | Homogeneous numeric vectors | library, no reader syntax | library | S7 | `u8vector` is the bytevector type; the other nine are new record types with range checks. As in Racket, no `#s16(...)` syntax (`#u8(...)` is R7RS's own) |
| 5 | `let` with signatures and rest args | library | library | S8 | gated arm on the `let` lowering (D3) |
| 6 | Basic String Ports | re-export | built in | S1 | `(scheme base)` |
| 7 | Feature-based program configuration | library | library | S8 | `program` lowers onto `cond-expand`, `include` and `import` |
| 8 | RECEIVE | library | library | S2 | `syntax-rules` over `call-with-values` |
| 9 | Defining Record Types | library | built in | S1 | R7RS `define-record-type` is SRFI 9's |
| 11 | Syntax for receiving multiple values | library | built in | S1 | R7RS `let-values` takes dotted rest formals (Racket's core does not) |
| 13 | String Libraries | library | library | S5 | needs 14; works over code-point vectors (2.5); `string-map`/`string-for-each` conflict with base (D5) |
| 14 | Character-set Library | library | library | S5 | inversion lists; standard sets from the Unicode 16 tables, with General Category data added for punctuation/symbol/title-case |
| 16 | Syntax for procedures of variable arity | re-export | built in | S1 | `(scheme case-lambda)` |
| 17 | Generalized `set!` | library | library | S2 | gated `set!` arm; setters for `car`, `cdr`, `vector-ref`, `string-ref`, `bytevector-u8-ref`, the `c[ad]r` family, later `hash-table-ref` |
| 19 | Time Data Types and Procedures | library | library | S8 | large: dates, julian days, TAI/UTC with a leap-second table, `date->string`; the one C-heavy SRFI |
| 23 | Error reporting mechanism | re-export | built in | S1 | R7RS `error` is SRFI 23's |
| 25 | Multi-dimensional Array Primitives | library | library | S8 | reference implementation; names clash with 63 |
| 26 | `cut`/`cute` | library | library | S2 | reference implementation, `syntax-rules` |
| 27 | Sources of Random Bits | library | library | S7 | the reference MRG32k3a generator in Scheme: saveable state, bignum ranges |
| 28 | Basic Format Strings | re-export (Racket's `format`) | library | S6 | not in R7RS, so not a no-op here; shares 48's engine |
| 29 | Localization | library | library | S8 | bundles in 69 tables; language and country from `LANG` |
| 30 | Nested Multi-line Comments | empty module | built in | S1 | `#\| \|#` is R7RS syntax; empty export list, as Racket's |
| 31 | `rec` | library | library | S2 | `syntax-rules` |
| 34 | Exception Handling for Programs | library | built in | S1 | R7RS `guard`/`raise`/`with-exception-handler` are SRFI 34's |
| 35 | Conditions | library | library | S7 | records; whether R7RS error objects answer `&error`/`&message` is decided in S7 |
| 38 | External Representation for Data With Shared Structure | library | alias | S1 | `write-with-shared-structure` is `write-shared`; `read-with-shared-structure` is `read` |
| 39 | Parameter objects | re-export | built in | S1 | converter semantics probed equal (2.2) |
| 40 | A Library of Streams | library | not planned | -- | deprecated by its author in favour of 41; the error says `(import (srfi 41))` |
| 41 | Streams | library | library | S7 | reference implementation over records and `delay-force` |
| 42 | Eager Comprehensions | library | library | S7 | needed leading-colon identifiers (2.3, since resolved); a stress test for the expander |
| 43 | Vector Library | library | library | S8 | index-first `vector-map` conflicts with base (D5); SRFI 133 is its R7RS-compatible successor (Section 5) |
| 45 | Primitives for Expressing Iterative Lazy Algorithms | library | alias | S1 | `lazy` is `delay-force`, `eager` is `make-promise` |
| 48 | Intermediate Format Strings | library | library | S6 | superset of 28 |
| 54 | Formatting | library | library | S8 | `cat`; reference implementation |
| 57 | Records | library | library | S8 | large `syntax-rules` reference implementation |
| 59 | Vicinity | library | library | S8 | `implementation-vicinity` is the stdlib directory |
| 60 | Integers as Bits | library | library | S7 | fixnums over Turmeric bit ops; bignums in Scheme (2.5); negatives in two's complement |
| 61 | A more general `cond` clause | library | library | S2 | gated `cond` arm |
| 62 | S-expression comments | no module | no library | S1 | `#;` is always on; the import is an error saying so, as in Racket |
| 63 | Homogeneous and Heterogeneous Arrays | library | library | S8 | clashes with 25 |
| 64 | A Scheme API for test suites | library | library | S6 | the runner exits non-zero after a failure, so `tur test` sees it; `tur init --r7rs`'s test uses it |
| 66 | Octet Vectors | library | alias | S7 | `u8vector-*` over bytevectors, plus `u8vector=?`/`u8vector-compare` |
| 67 | Compare Procedures | library | library | S8 | reference implementation |
| 69 | Basic hash tables | library | library | S4 | Scheme-level buckets; `equal-hash` agrees with `equal?` |
| 71 | Extended LET-syntax for multiple values | library | library | S8 | gated `let` arms |
| 74 | Octet-Addressed Binary Blocks | library | library | S8 | a blob is a bytevector |
| 78 | Lightweight testing | library | library | S6 | `check-ec` follows 42 (S7) |
| 86 | MU and NU | library | library | S8 | only if the reference implementation runs as written |
| 87 | `=>` in `case` clauses | library | built in | S1 | R7RS `case` has `=>` |
| 98 | Interface to access environment variables | library | built in | S1 | re-exports `(scheme process-context)`'s two procedures |
| 105 | Curly-infix-expressions | no module | built in, no library | -- | `{a + b}` reads in every dialect, `#lang r7rs` included |

(46, custom ellipsis, is R7RS 4.3.2 and has no row in either list; it is built
in, `:::` included since the colon report was resolved.)

The guide's version gets a short preamble:

- how to import (`(import (srfi 1))`);
- what "built in" means ("the import is accepted and costs nothing, because
  R7RS already includes this SRFI");
- that importing a built-in SRFI next to `(scheme base)` is fine;
- that `cond-expand` knows `srfi-N`.

It also gets one line on SRFI 62.

---

## 5. Beyond Racket's list (not scheduled)

These are what current R7RS code reaches for, and several are R7RS-large
libraries. None is in Racket's `srfi` collection. Each is one more table row,
one file and one fixture on the machinery S1 builds, taken when asked for:

- 111 boxes;
- 113 sets and bags;
- 115 regular expressions (an adaptor over stdlib/re.tur);
- 117 list queues;
- 125 hash tables, sharing 69's core;
- 128 comparators;
- 130 and 152 strings (cursor-based, which suits UTF-8 better than 13);
- 132 sorting;
- 133 vectors, the R7RS-compatible 43;
- 141 integer division;
- 143 and 144 fixnums and flonums;
- 151 bitwise operations, sharing 60's core;
- 158 generators and accumulators.

---

## 6. Stages

### S0 -- inventory and measurements (small)

- For each row: does the SRFI document or chibi ship a test suite, and what is
  the reference implementation's licence? Record both in the table's source
  comments.
- Measure what splicing costs. `#lang r7rs` programs already build slowly
  ([docs/reported/r7rs-programs-compile-slowly.md](../reported/r7rs-programs-compile-slowly.md)),
  and every procedure a library splices in is emitted and C-compiled into the
  importing program. Take SRFI 1's reference implementation (about 150
  procedures), splice it into a one-line program, and time `tur build` with and
  without it. If the cost is material, either drop unreferenced spliced
  definitions before emission (check first whether the emitter already prunes
  them) or split the big SRFIs into several files.
- Decide the S1 questions in Section 7.

### S1 -- the mechanism, the built-ins, the table (medium)

- Integer library-name parts (D1); `SRFI_LIBS[]` (D4); inline-mode
  `define-library` (D3); splicing through `scheme_import_library_files`.
- The built-in and alias rows: 6, 9, 11, 16, 23, 30, 34, 38, 39, 45, 87, 98.
  The no-library row (62), the not-planned row (40), and "not yet" for every
  other row, each with its message.
- `cond-expand` `(library (srfi N))`, `srfi-N` feature ids, `(features)`
  generated from the table (closes the `ratios` report).
- The D5 conflict check, with fix text. The first incompatible row lands
  later, so S1 pins it with a unit test of the check itself.
- `tests/check-r7rs-srfi-sync.sh`.
- The guide section with the table (Section 4, as of S1); "What is there"
  gains a line pointing to it; r7rs-lang-plan's See also links here.
- Fixtures:
  - `r7rs-srfi-builtins`: every built-in and alias row imported next to
    `(scheme base)`, one name from each exercised, under
    `only`/`except`/`prefix`/`rename` as well;
  - `r7rs-srfi-cond-expand`;
  - `r7rs-srfi-macro-two-passes`: a user library and the program both import
    `(srfi 45)` and both use its `lazy` macro. That pins D3's per-pass macro
    registration before any macro-heavy SRFI depends on it;
  - `errors/r7rs-srfi-unknown`, `errors/r7rs-srfi-62-no-library`,
    `errors/r7rs-srfi-40-not-planned` and `errors/r7rs-srfi-not-yet`.

**Exit criterion:** a program importing every built-in row emits the same C,
byte for byte (`tur emit-c`), as the same program without those imports. That
is the no-op made literal, checked by a small script next to the fixture. The
fixture itself runs on both back ends.

### S2 -- the small syntax SRFIs (small)

- 2, 8, 26 and 31 as `syntax-rules` files, reference implementations where
  portable.
- 61 and 17 as gated arms on `cond` and `set!`. SRFI 17's `setter` is a table
  keyed by procedure identity: procedures keep identity, as the Eval section
  of the guide notes. Turmeric's own `(set! (.field x) v)`, which the prelude
  uses, is untouched, because its head is a `.field`, not a Scheme identifier.
- A fixture per SRFI, and the SRFI's own tests where S0 found them.

### S3 -- SRFI 1 (medium)

- The reference implementation, ported under D3's spellings, with the
  base-compatible names bound to the prelude's procedures (D5). Confirm each
  one's edge cases match SRFI 1: `map` over unequal lengths, `member`/`assoc`
  with `=`, `list-copy` of an improper list.
- `length+` and the circular-list procedures against the prelude's `list?`
  cycle check.
- The build-time delta from S0, re-measured on the real file.

### S4 -- SRFI 69 hash tables (medium)

- Representation: a record holding a vector of association-list buckets, with
  the equality predicate and hash function, resized by load factor. It is all
  Scheme values, so the collector sees it and no region hook is needed (2.5).
- New primitives, with interpreter twins:
  - `equal-hash`, which must agree with `equal?`. A mutable string and a
    literal with the same characters hash equal, and a bignum or ratio hashes
    its normalized value. It is written as one C walk over the value, or in
    Scheme over the prelude's type predicates if that is fast enough (measure);
  - `string-hash` and `string-ci-hash` (the latter over the Unicode fold the
    prelude already has);
  - `hash-by-identity` (`eq?`: the address, stable under the non-moving
    collector, and the value for an immediate).
- `hash-table-ref`'s failure thunk, `hash-table-update!/default`,
  `hash-table-walk`, `hash-table-fold` and the rest of the SRFI.

### S5 -- SRFI 14, then 13 (large)

- 14: char sets as sorted inversion lists over code points, and the standard
  sets. `char-set:letter`, `digit`, `whitespace`, `upper-case` and
  `lower-case` come from stdlib/r7rs/unicode.tur's tables. `punctuation`,
  `symbol` and `title-case` need General Category data: extend
  `tools/fetch-ucd.sh`'s output and its sync check.
- 13: over a code-point vector made once per call (`string->vector`), not
  repeated `string-ref` (2.5); results are fresh mutable strings. Default
  arguments that are char sets use 14. `string-map` and `string-for-each`
  conflict with base (D5).

### S6 -- formatting and testing (medium)

- 28 and 48 from one `format` engine written over string ports. 28 is the
  subset (`~a ~s ~% ~~`).
- 64 test suites: the default runner prints SRFI 64's summary, and the
  outermost `test-end` exits non-zero when anything failed. `tur test` passes a
  file only when it builds and exits 0 (src/main.c), so a failing SRFI 64
  test then fails `tur test`. `tur init --r7rs`'s scaffolded test switches
  from a `display` to `(import (srfi 64))`, which gives newcomers a real test
  file on day one.
- 78 `check`, `check-report` and friends. `check-ec` waits for 42.

### S7 -- data and control (medium each)

- 27: the reference MRG32k3a generator; `random-source-randomize!` seeds
  from `current-jiffy`.
- 35: condition types as records.
- 41: the reference implementation.
- 42: the colon report that blocked it is resolved; a large `syntax-rules`
  workout, and any expander limit it hits is a conformance finding.
- 60: fixnums via Turmeric bit ops, bignums arithmetically.
- 4 and 66: bytevector aliases and the typed vectors.
- 78's `check-ec`.

### S8 -- the long tail (on demand)

These are not scheduled, and a row flips when someone asks for it: 5, 7, 19,
25, 29, 43, 54, 57, 59, 63, 67, 71, 74, 86. Each is a file, a fixture, and a
row in the table.

---

## 7. Open questions

1. **SRFI 62: an error (Racket) or a no-op?** This plan says an error, per the
   ask's "if Racket does same". The message makes it harmless. It is one
   table cell either way.
2. **`(srfi :1)` and `(srfi 1 lists)`.** SRFI 97's R6RS-era spellings, which
   some portable code uses. Racket's R7RS rejects them; so does this plan
   until someone needs them.
3. **Turmeric importing an SRFI.** `(import srfi/1 ...)` from a `.tur` file
   would need the SRFI libraries as modules, which D3 avoids. Out of scope. A
   Turmeric program has the typed stdlib, and a Scheme library can wrap an
   SRFI for it.
4. **SRFI 35 and R7RS error objects.** Should `(condition-has-type? e &error)`
   hold for an `error` object, and `error-object?` for an SRFI 35 `&error`
   condition? Chibi and Gauche differ. Decide in S7.
5. **SRFI 69's `hash`.** The SRFI's `hash` takes an optional bound; R7RS-era
   code often expects SRFI 128's `default-hash`. Keep 69's names exact, and let
   125/128 (Section 5) add theirs over the same primitives.

---

## 8. Risks

- **Build time.** Covered in S0. It is the one risk that could change D3's
  shape (per-feature files, or pruning).
- **Macros across lowering passes.** D3 registers a spliced SRFI's macros in
  every pass that imports it. If that fails, a user library that uses
  `receive` breaks while the program does not. S1's two-pass fixture pins it
  before any macro SRFI ships.
- **Our `syntax-rules` against real reference implementations.** SRFI 42, 57
  and 86 use the pattern language hard. Some of what they hit will be expander
  bugs, which is useful, but it can stall a stage. Those rows are S7/S8 for
  that reason.
- **Interpreter parity.** Every fixture runs on both back ends, and every
  inline-C primitive has its twin. D3's "Scheme first" keeps the twins to a
  handful.
- **GC and regions.** SRFI data structures are made of Scheme values. Any
  that are not must be rooted for the collector and noted for regions
  (CLAUDE.md's region store rule), and they get a fixture under
  `TUR_GC_TORTURE`.
- **The web REPL.** SRFI files are stdlib files and must reach the WASM
  bundle, as stdlib/r7rs/*.tur do. S1 checks this in the browser REPL.

---

## Appendix A -- probe transcript

2026-09-26, fdd51fc9, `cmake -DCMAKE_BUILD_TYPE=Debug`. Each program ran as
`tur run p.tur` and as `tur --interpret p.tur` (the latter with
`ASAN_OPTIONS=detect_leaks=0`, since the interpreter keeps its values for the
life of the process by design, per CLAUDE.md). The two outputs were identical
in every case.

**A.1 -- the SRFI import today.**

```scheme
#lang r7rs
(import (scheme base) (scheme write) (srfi 1))
(write (fold + 0 '(1 2 3)))
```
```
srfi1.tur:2:44: error: a library name part must be an identifier
srfi1.tur:3:9: error: unknown function or operator 'fold'
```

**A.2 -- the built-ins (2.2).**

```scheme
#lang r7rs
(import (scheme base) (scheme write))
#| nested #| comment |# |#
(write (list 1 #;(ignored) 2)) (newline)
(write (case 5 ((5) => (lambda (x) (* x 10))) (else 0))) (newline)
(let-values (((a . rest) (values 1 2 3))) (write (list a rest))) (newline)
(define p (make-parameter 10 (lambda (x) (* x 2))))
(write (list (p) (parameterize ((p 3)) (p)))) (newline)
(write (features)) (newline)
(write {1 + 2}) (newline)
```
```
(1 2)
50
(1 (2 3))
(20 6)
(r7rs exact-closed ratios turmeric)
3
```

```scheme
#lang r7rs
(import (scheme base) (scheme write) (scheme process-context) (scheme lazy) (scheme read))
(write (guard (e (#t (list 'caught e))) (raise 'boom))) (newline)
(write (guard (e ((assq 'a e) => cdr) ((assq 'b e))) (raise (list (cons 'a 42))))) (newline)
(write (force (delay-force (delay 7)))) (newline)
(write (force (make-promise 8))) (newline)
(write (string? (get-environment-variable "HOME"))) (newline)
(define-record-type point (make-point x y) point? (x point-x set-point-x!) (y point-y))
(define pt (make-point 1 2)) (set-point-x! pt 5)
(write (list (point? pt) (point-x pt) (point-y pt))) (newline)
(write ((case-lambda ((a) 'one) ((a b) 'two)) 1 2)) (newline)
(let ((l (list 1 2))) (set-cdr! (cdr l) l) (write-shared l)) (newline)
```
```
(caught boom)
42
7
8
#t
(#t 5 2)
two
#0=(1 2 . #0#)
```

```scheme
#lang r7rs
(import (scheme base) (scheme write))
(let ((out (open-output-string)))
  (write 'hi out)
  (write (list (get-output-string out) (read-char (open-input-string "z")))))
(newline)
(write (guard (e ((error-object? e) (list (error-object-message e) (error-object-irritants e))))
         (error "bad" 1 2)))
(newline)
```
```
("hi" #\z)
("bad" (1 2))
```

With `(scheme read)` imported,
`(read (open-input-string "#0=(a b . #0#)"))` gives a list whose `cddr` is
`eq?` to itself: `(list (car x) (eq? x (cddr x)))` writes `(a #t)`.

**A.3 -- a spliced file carries macros, hygienically (2.4).** `util.scm`:

```scheme
(define-syntax my-receive
  (syntax-rules ()
    ((_ formals expr body ...) (call-with-values (lambda () expr) (lambda formals body ...)))))
(define (helper x) (* x 100))
(define-syntax use-helper (syntax-rules () ((_ x) (helper x))))
```
```scheme
#lang r7rs
(import (scheme base) (scheme write))
(load "util.scm")
(my-receive (a b . c) (values 1 2 3 4) (write (list a b c))) (newline)
(let ((helper (lambda (x) 'captured))) (write (use-helper 7))) (newline)
```
```
(1 2 (3 4))
700
```

The same macro file `include`d by a user library and `load`ed by the program:
both uses expand (`(200 300)`). A program and a user library that both import
`(scheme time)`: both work (`(#t #t)`).

**A.4 -- the three defects (2.3).** The colon and `ratios` repros are in their
reports. The macro-export repro:

```
./mymac.tur:2:1: error: exported symbol 'my-rec' is not defined in this module
```

---

## Appendix B -- Racket evidence

Read 2026-09-26 from `raw.githubusercontent.com`:

- `racket/srfi`, `srfi-doc/srfi/scribblings/srfi.scrbl`: the 49 documented
  SRFIs; `@in-core{}` ("This SRFI's bindings are also available in
  racket/base") on 6, 11 ("but without support for dotted 'rest' bindings"),
  16, 23, 28 and 39; "This SRFI's syntax is part of Racket's default reader"
  on 30 and 38 (38 adds "and printer"); SRFI 62's section is marked
  `@; no actual library for this`.
- `racket/srfi`, `srfi-lib/srfi/6.rkt`, `16.rkt`, `23.rkt`, `28.rkt`: one
  `provide` of the core names each. `9.rkt`, `34.rkt`, `38.rkt`, `87.rkt`:
  their own implementations (`#lang s-exp srfi/provider ...`); `11.rkt`: its
  own `let-values` with rest arguments. `srfi-lib/srfi/30.rkt`: `;; Supported by core PLT,
  nothing to provide`. `srfi-lib/srfi/39.rkt`: a re-export with a guard
  wrapper. `srfi-lib/srfi/62.rkt`: absent. Also absent from `srfi-lib` and
  `srfi-lite-lib`: 0, 46, 105, 110, 111, 113, 125, 128, 133, 141, 151, 158.
- `lexi-lambda/racket-r7rs`, `r7rs-lib/private/import.rkt`: a
  `library-name-element` is an `id` or an `integer`, and a non-`scheme`
  library name becomes the module path of its elements joined with `/`.

---

## See also

- [r7rs-lang-plan.md](r7rs-lang-plan.md) -- the dialect this extends; D9 (the
  library system) and R7 (the on-demand library mechanism S1 reuses).
- [docs/guides/r7rs-guide.md](../guides/r7rs-guide.md) -- where the support
  table goes.
- [docs/guides/experimental-flags-guide.md](../guides/experimental-flags-guide.md)
  -- the rule D8 reasons about.
