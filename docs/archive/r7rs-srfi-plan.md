# SRFI libraries for `#lang r7rs`, after Racket's

> **Status:** **complete** -- S1 and S0 landed 2026-09-26; S2 (SRFIs 2, 8,
> 26, 31 and 61), S3 (the pruning pass and SRFI 1), S4 (SRFI 69), S5 (SRFIs
> 14 and 13), S6 (SRFIs 28, 48, 64 and 78) and S7 (SRFIs 4, 27, 35, 41, 42,
> 60 and 66, and 78's `check-ec`) landed 2026-09-27 (see their "What
> shipped" and "What S0 found" notes). S2's last SRFI, 17, waited on
> docs/archive/r7rs-prelude-procedures-lose-identity.md (fixed 2026-09-27)
> and landed 2026-09-28 (3ed9f9b65). S8 is on demand by decision, not
> unstarted work: its fourteen rows (5, 7, 19, 25, 29, 43, 54, 57, 59, 63,
> 67, 71, 74, 86) stay "not yet (S8)" in `SRFI_LIBS[]` and the guide, and
> each flips when someone asks; Section 5 is unscheduled likewise. All five
> Section 7 questions are decided. The one report filed on the way,
> letrec-mutual-recursion-between-capturing-closures (S6), was resolved
> 2026-09-28 and is in docs/archive/ too; the body's other docs/reported/
> mentions (type-error panics, too-few arguments, procedure identity) were
> fixed 2026-09-27 and are in docs/archive/. Archived 2026-09-28.

`(import (srfi N))` resolves for every SRFI in the table: the ten built-in
rows, the three alias rows and the twenty library rows import, and the
rest are refused with their reason. S0's inventory is [Appendix C](#appendix-c----s0-inventory). Its
measurement said a big SRFI needs its unreferenced definitions dropped
before emission. S3 built that first, and an unused `(import (srfi 1))` now
costs nothing. Every "today" claim in Sections 1-2 was measured on
2026-09-26 against `./build/tur` at fdd51fc9 (Debug build), on both back ends
(`tur run` and `tur --interpret`), before S1. The transcript is in
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
  [docs/archive/r7rs-turmeric-syntax-leaks.md](../archive/r7rs-turmeric-syntax-leaks.md))
- **A `define-library` cannot export a `syntax-rules` macro.** The export check
  in src/compiler/elab_module.c refused it: "exported symbol 'my-rec' is not
  defined in this module". That ruled out the obvious design of SRFI
  libraries as ordinary library modules (D3). Resolved since: a library
  exports its macros, and an importer reads them from the library's source.
  [docs/archive/r7rs-define-library-cannot-export-syntax.md](../archive/r7rs-define-library-cannot-export-syntax.md)
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
user library: `except` that hides nothing (guide, "Where it differs"),
exports typed `any` on the Turmeric side, and a separate compile per library.
(No exported syntax was on this list too, until that gap closed, 2.3.) The table route inherits the `(scheme ...)` libraries'
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

Each supported SRFI is one file, `stdlib/srfi/<N>.scm`, holding one
`(define-library (srfi N) ...)` written in plain R7RS. (Not under
`stdlib/r7rs/`: files there are Turmeric-shaped, and the lowering exempts
them from Scheme semantics; see S1's "What shipped".) That mirrors Racket's
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

This lets SRFI reference implementations port with small edits, checkable
against chibi or Racket. It was first chosen to sidestep the macro-export gap
(2.3). That gap is now closed for user libraries, and the closing reused the
same idea: an importer reads the library's source for its macros. Inline mode
still wins for SRFIs on D1's other grounds, and because a spliced library's
macros and procedures are one lowering pass with its importer.

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
  SRFI 0 introduced. `feature_holds` reads it from the table. `(features)`'s
  list is written out in the prelude, and the sync check below fails when it
  differs (S1's "What shipped" says why it is not generated);
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
  header, with a licence note in `stdlib/srfi/COPYING` as
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
  `tur_r7rs_conformance` does. S0 found which suites exist
  ([Appendix C](#appendix-c----s0-inventory)).
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
| 17 | Generalized `set!` | library | library | S2 | gated `set!` arm; setters for `car`, `cdr`, `vector-ref`, `string-ref`, `bytevector-u8-ref` and the `c[ad]r` family, plus `getter-with-setter` and `(set! (setter f) s)`. Landed 2026-09-28. Was blocked: `setter` is keyed on procedure identity, which the standard procedures lost until 2026-09-27 (docs/archive/r7rs-prelude-procedures-lose-identity.md). `hash-table-ref` has no setter: SRFI 69 does not ask for one, and an entry for it would make every `(srfi 17)` import carry SRFI 69 |
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
  ([docs/archive/r7rs-programs-compile-slowly.md](../archive/r7rs-programs-compile-slowly.md)),
  and every procedure a library splices in is emitted and C-compiled into the
  importing program. Take SRFI 1's reference implementation (about 150
  procedures), splice it into a one-line program, and time `tur build` with and
  without it. If the cost is material, either drop unreferenced spliced
  definitions before emission (check first whether the emitter already prunes
  them) or split the big SRFIs into several files.
- Decide the S1 questions in Section 7.

> **What S0 found (2026-09-26).**
>
> **The inventory** is [Appendix C](#appendix-c----s0-inventory), one row per
> SRFI. It is not in `SRFI_LIBS[]`'s source comments as this stage first
> said, because 51 rows of provenance would drown the table. The table's
> comment points at the appendix instead. Three things in it change later
> stages:
>
> - **S3 ports chibi's SRFI 1, not the reference.** chibi's is an R7RS
>   library (BSD-3, the licence already vendored as
>   `tests/r7rs/CHIBI-COPYING`), 492 lines in ten files, with 156
>   `test` forms in its `test.sld`. Its `(else ...)` branch, the one for a
>   Scheme that is not chibi, was spliced unmodified into a program with
>   `include`. It gave the SRFI's answers for a 20-call sample on both back
>   ends (fold, iota, delete-duplicates, lset-union, partition and span
>   through `call-with-values`, unfold, alist-delete, ...). The reference
>   (Shivers) is 1,596 lines and leans on `:optional`, `let-optionals` and
>   `check-arg`.
> - **SRFI 13 and 14's reference implementations carry the old MIT Scheme
>   licence.** Its clause 2 asks users to "make their best efforts" to return
>   improvements to MIT, and clause 3 asks for acknowledgement. That is
>   permissive but unusual. D6 already writes 13 ourselves over code-point
>   vectors. For 14, port chibi's (BSD, with a `test.sld`); the reference
>   repository's `srfi-14-tests.scm` is still usable as a test suite.
> - **Test suites exist for 1, 2, 14, 26, 27, 35, 41 and 69 in chibi, and
>   for 4, 14, 19, 25, 26, 27, 41, 48, 64 and 67 in the SRFI repositories.**
>   chibi also has suites for 16 and 38, which are built in here, so they are
>   a free check of S1's claim.
>
> **The measurement.** A Debug (ASan) `tur`, a 4-core container with nothing
> else running, three runs each; the medians. `cc` is the C compile inside
> `tur build`, timed through a `CC` wrapper:
>
> | Program | `tur emit-c` | `cc` | `tur build` | C lines | C functions |
> |---|---|---|---|---|---|
> | `p0`: `(write 1)` | 0.91 s | 2.5 s | 3.46 s | 28,824 | 2,883 |
> | `p1`: `p0` with chibi's SRFI 1 body spliced in, none of it called | 1.33 s | 3.8 s | 5.03 s | 34,185 | 3,342 |
> | `p2`: `p1` calling `fold` | 1.37 s | -- | 5.06 s | 34,188 | 3,340 |
> | `p3`: `p0` with its own 1-line `fold` | -- | 2.7 s | 3.66 s | -- | -- |
>
> Splicing SRFI 1 costs every importing program about 1.6 s (+45%), whether
> it calls one procedure or none. `tur --interpret` does not notice
> (0.42 s -> 0.45 s). That is material.
>
> **The emitter does not prune, and neither can gcc.** Every spliced
> definition reaches the C, used or not (`lset-xor` is emitted in `p1`). gcc
> already discards the prelude's unused functions before optimizing (see
> [docs/archive/r7rs-programs-compile-slowly.md](../archive/r7rs-programs-compile-slowly.md)),
> but a spliced Scheme library defeats it. `__tur_fatbox_init` runs at
> startup and fills a static closure for every procedure the program uses
> as a value *anywhere*, including in dead code: `every` calls
> `(apply any ...)`, so `any` has a fatbox. Every variable define
> (`(define reverse! reverse)`) is initialized at startup too. Both hold
> function pointers, so gcc keeps those functions and everything they call.
> Rewriting the body's 19 one-line aliases (`(define reverse! reverse)`,
> `(define first car)`, ...) as procedures still left 190 of its 318 new
> functions alive (`-fdump-ipa-cgraph`), nearly all of them lifted lambdas.
>
> **Decision: prune, in a whole-program pass before emission; do not
> split.** Drop every definition from a `stdlib/srfi/` file that nothing
> outside the SRFI files reaches, transitively through the SRFI's own
> definitions. The whole program has to be visible for this, so it cannot
> happen in a lowering pass: an SRFI is spliced once per compile, in the
> first pass that imports it, and a library lowered later may use a
> procedure the program does not (S1's `srfi-in-library-only`). It can
> happen after elaboration, where every module is loaded and the program is
> one translation unit. `p3` is the estimate of what it wins: a program
> using `fold` should build within about 0.2 s of `p0`. Splitting loses:
> a plain `(import (srfi 1))` keeps every export, so splitting helps only an
> `only` import; and SRFI 1's parts call each other (the `lset-*` procedures
> call `filter` and `remove`; `delete` calls `remove`), so even that would
> pull in most files. The pass lands with S3, ahead of the SRFI 1 file, and
> S3 re-measures on the real file.
>
> **A defect the measurement tripped over, fixed.** The spliced body defines
> `any`, and the compiled program then failed inside the prelude: the
> stdlib's `: any` annotations read the program's procedure. The same held
> for every Turmeric type name (`int`, `ptr`, ...), for stdlib names
> (`None`, `Vec`; interpreted as well), and, differently, for a program's
> own `square` or `list` ("'r7rs-square' is already defined"). A program's
> definition is now its own, whatever the name: a type name is spelled
> `<name>--user` wherever user code names it, a stdlib name is respelled,
> and a standard name is shadowed for the program, as chibi allows. At the
> REPL, the respelling carries across turns. Pinned by
> `tests/fixtures/r7rs-program-shadows-names`,
> `r7rs-repl-shadowed-name-persists` and `run-r7rs-import.sh`'s
> `type-named-exports`. The library half, a `define-library` that defines
> `square` or `None`, is
> [docs/archive/r7rs-library-defines-standard-or-stdlib-name.md](../archive/r7rs-library-defines-standard-or-stdlib-name.md)
> (fixed 2026-09-28).
> An SRFI file is untouched by it, since its names are spelled
> `srfi<N>--<name>` (D3).
>
> **Section 7's S1 questions** (1-3) are decided; 4 and 5 stay with S7 and
> S4, where they belong.

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

> **What shipped (2026-09-26).** Everything above, with these differences from
> the plan as written. The exit criterion holds:
> `r7rs-srfi-builtins-emit-nothing` compares `tur emit-c` with and without
> the ten built-in imports (29,196 lines, identical).
>   The stream walks in `r7rs-srfi-builtins` (10,000 steps) and the guide's
>   example (1,000) are sized for `run-r7rs-gc.sh`, which runs every r7rs
>   fixture with a collection every 31 allocations; 100,000 took 124 s there
>   alone and the guide fixture, which links the evaluator, over 600 s.
>
> - **Where the files are.** `stdlib/srfi/<N>.scm`, not
>   `stdlib/r7rs/srfi/<N>.tur`. `prelude_span` treats everything under
>   `stdlib/r7rs/` as Turmeric-shaped: no rename table, no operator rewrite,
>   Turmeric's lexemes. An SRFI file is real Scheme, so it lives beside that
>   directory, and `srfi_span` marks it instead. The release archives and the
>   WASM bundle take `stdlib/` whole, so nothing else changed to ship it.
> - **"Inline mode" is a pre-pass.** `srfi_source_forms` turns a spliced SRFI
>   file's `define-library` into its body's forms before any scan reads the
>   program. Every name is spelled onto its target: the body's definitions
>   become `srfi<N>--<name>`, and an on-demand `(scheme ...)` import's names
>   become that library's procedures, so the SRFI's import does not make them
>   visible to the program. The lowering emits those forms in place, like the
>   prelude's, outside a program's module wrapper, so every module of the
>   compile sees them. Macros are left out of the splice. Every lowering pass
>   that imports the SRFI registers them (`srfi_import` / `SrfiLib`, the
>   same idea as a user library's macro export), under hidden spellings with
>   the import set's names as aliases. A re-export of R7RS syntax under a new
>   name (`(rename (srfi 87) (case kase))`) becomes a forwarding macro.
> - **An SRFI file imports `(scheme ...)` libraries only, for now.** SRFI 13
>   importing SRFI 14 (S5) needs the pre-pass to spell another SRFI's names
>   into the body too. It refuses anything else until then. (S5 did that:
>   see its note.)
> - **`(features)` is written out, not generated.** The prelude is
>   Turmeric-shaped and is loaded unlowered when a Turmeric program imports a
>   Scheme library, so a placeholder the lowering fills in broke every
>   Turmeric importer (caught by `run-r7rs-import.sh`). Its list stays
>   written out. `tests/check-r7rs-srfi-sync.sh` fails when it differs from
>   `R7RS_FEATURES` plus `srfi-N` for every supported row, and
>   `r7rs-features-agree` asks `cond-expand` about each entry at run time.
> - **D5 is pinned by fixtures, not a unit test.** No incompatible SRFI has
>   landed yet, but a rename onto an R7RS name shows the check:
>   `errors/r7rs-srfi-conflict` renames SRFI 45's `eager` to `force` next to
>   `(scheme base)`. `errors/r7rs-srfi-redefine` (defining an imported name)
>   and `errors/r7rs-srfi-not-exported` (an import set naming a non-export)
>   ride along.
> - **The two-pass macro fixture is two `run-r7rs-import.sh` cases**
>   (`srfi-in-program-and-library`, `srfi-in-library-only`), since a
>   multi-module test needs that runner.
> - **Integer library-name parts** work for user libraries too:
>   `(import (mylib 2))` is the module `mylib/2`, as in Racket's R7RS.
> - **Found and fixed on the way:** the load expander took at most eight
>   library files from one `import` form and silently dropped the rest
>   (`libs[8]` in elab_toplevel.c). A program importing twelve SRFIs lost
>   four. It takes 128 now.
> - **SRFI 45's `eager` is `make-promise`.** R7RS adopted it that way, and
>   `delay` of a promise chains here too. So `(force (eager (delay 7)))` is 7,
>   where SRFI 45's reference implementation would give the inner promise.
>   stdlib/srfi/45.scm says so.
>
> S0's inventory and build-time measurement did not come first, because S1
> needed neither. Both landed after it, the same day (S0's "What S0 found").

### S2 -- the small syntax SRFIs (small)

- 2, 8, 26 and 31 as `syntax-rules` files, reference implementations where
  portable.
- 61 and 17 as gated arms on `cond` and `set!`. SRFI 17's `setter` is a table
  keyed by procedure identity: procedures keep identity, as the Eval section
  of the guide notes. Turmeric's own `(set! (.field x) v)`, which the prelude
  uses, is untouched, because its head is a `.field`, not a Scheme identifier.
- A fixture per SRFI, and the SRFI's own tests where S0 found them.

> **What shipped (2026-09-27): 2, 8, 26, 31 and 61. 17 followed on
> 2026-09-28.**
>
> - **2, 8, 26, 31** are `syntax-rules` libraries:
>   - 2 is chibi's `and-let*` (BSD-3);
>   - 8's `receive` and 31's `rec` are the SRFI documents' own definitions
>     (MIT);
>   - 26 is the reference `cut.scm` (public domain).
>
>   `stdlib/srfi/COPYING` records each file's origin and carries the
>   licences.
> - **61** is an arm of the lowering's `cond` (`srfi61_clause`). It is on in
>   a unit that imports the SRFI's `cond`, under any name. The clause lowers
>   as the SRFI's reference implementation does: `call-with-values` into a
>   consumer that applies guard, then receiver. The plumbing is built from
>   global aliases, so a local `apply` or `lambda` at the use site does not
>   capture it. The export is R7RS's own `cond`, so the import sits beside
>   `(scheme base)` as one binding. Without the import, the clause shape is
>   an error that names the import.
> - **The SRFIs' own suites (D7).** `tests/r7rs/run-conformance.py` takes
>   `--suite`, `--import` and `--label`. `tests/run-r7rs-srfi-suites.sh`
>   (ctest `tur_r7rs_srfi_suites_1` and `_2`, a shard each since S7) runs
>   each `tests/r7rs/srfi/<N>/tests.scm` against the floor in
>   `tests/r7rs/srfi/<N>/floor`:
>   - SRFI 2 runs chibi's suite, 31/31 on both back ends;
>   - SRFI 26 runs chibi's suite plus the reference `check.scm`, 26/26.
> - **A hygiene fix SRFI 26 needed** (R7RS 4.3.2). The reference `cut`
>   inserts `x` at each recursive step, and only the last step makes them
>   lambda parameters. The expander renamed a template identifier only when
>   the same step bound it, so every step's `x` was one name:
>   `((cut list <> 'b <>) 'a 'c)` gave `(c b c)`. Now:
>   - an identifier a template passes to another macro use gets a per-step
>     alias (`hyg_pending`);
>   - lookups try an alias locally first;
>   - one step's aliases of one identifier are shared.
>
>   The old expander also let a helper macro's binder, passed on by a
>   template, capture the user's variable of the same name; that is fixed
>   too. `tests/fixtures/r7rs-hygiene-binder-made-later` pins it; it fails
>   three ways on the old expander.
> - **17 landed 2026-09-28**, once the identity fix it waited on was in
>   (docs/archive/r7rs-prelude-procedures-lose-identity.md): its `setter` is
>   an association list keyed by procedure identity, so `(setter car)` is
>   `set-car!` only because `(eqv? car car)` is `#t`.
>   - **The target is an arm of the lowering's `set!`** (`srfi17_place`), on
>     in a unit that imports this SRFI's `set!`, under any name -- the same
>     arrangement as 61's `cond`, so the export is R7RS's own `set!` and the
>     import sits beside `(scheme base)` as one binding. `(set! (f arg ...)
>     v)` becomes `((setter f) arg ... v)`, with `setter` reached through a
>     global alias of the SRFI's own definition, so a local `setter` at the
>     use site does not capture it and the program need not have imported
>     that name at all.
>   - **Without the import the shape is an error naming the SRFI**
>     (`errors/r7rs-srfi-17-not-imported`), where it used to reach Turmeric's
>     own "set! target must be a symbol, (@ borrow), or (.field struct)" --
>     one more r7rs-turmeric-syntax-leaks shape, closed in passing.
>     `(set! (.field x) v)` still means Turmeric's, since its head is not a
>     Scheme identifier.
>   - **Settable out of the box**: `car`, `cdr`, the 28 members of the
>     `c[ad]r` family, `vector-ref`, `string-ref`, `bytevector-u8-ref`, and
>     `setter` itself, which is what makes `(set! (setter f) s)` work.
>     `getter-with-setter` is the SRFI's.
>   - **An unused import costs nothing.** The table is built on first use, as
>     SRFI 14's char sets are: an initializer that allocates is not a
>     candidate for S3's pruning pass, so a table written as one `define`
>     would have pinned the whole `c[ad]r` family. Measured: `(import (srfi
>     17))` added to a `(write 1)` program emits byte-identical C.
>   - `tests/fixtures/r7rs-srfi-17` covers the standard setters, four levels
>     of `c[ad]r`, `getter-with-setter`, `(set! (setter f) s)`, the hygiene of
>     the plumbing and the `guard`-able error for a procedure with no setter,
>     on both back ends. SRFI 17 has no suite of its own (Appendix C).
> - **Reported on the way** (docs/reported/):
>   - `r7rs-dead-mistyped-call-refused-at-compile-time`: `(if x (car x) 0)`
>     with `x` bound to `#f` does not compile (fixed 2026-09-28, archived);
>   - `r7rs-type-errors-are-uncatchable-panics`: `(car 5)` at run time is a
>     panic `guard` cannot catch.

### S3 -- SRFI 1 (medium)

- First, the pruning pass S0 decided on: after elaboration and before
  emission, drop every `stdlib/srfi/` definition that nothing outside the
  SRFI files reaches, transitively. Its fixture compares `tur emit-c` for an
  SRFI 1 import that calls `fold` against one that calls nothing, and against
  a program that defines `fold` itself: the first two differ by `fold`'s
  definitions only.
- chibi's implementation (S0: an R7RS library, BSD-3, 492 lines, which ran
  unmodified), ported under D3's spellings, with the base-compatible names
  bound to the prelude's procedures (D5). Confirm each one's edge cases match
  SRFI 1: `map` over unequal lengths, `member`/`assoc` with `=`, `list-copy`
  of an improper list.
- `length+` and the circular-list procedures against the prelude's `list?`
  cycle check.
- chibi's `lib/srfi/1/test.sld` (156 tests) under `tests/r7rs/srfi/1/` (D7).
- The build-time delta from S0 (+1.6 s unpruned), re-measured on the real
  file with the pass on.

> **What shipped (2026-09-27).** Everything above, and two defects the work
> tripped over.
>
> - **The pruning pass** is `src/passes/srfi_prune.c`. `compile_to_c` runs
>   it on the final program, after the transform passes and just before
>   `emit_program`, so every path that emits a single translation unit
>   (`emit-c`, `build`, `run`, the JIT) is covered. Separate compilation and
>   the interpreter are left alone, and a REPL session keeps every
>   definition for later turns.
>   - **Candidates** are the top-level `defn`s and `def`s whose span is in a
>     `stdlib/srfi/` file, with the lambdas lifted out of them. A `def` is a
>     candidate only when its initializer has no effect (a literal, a
>     reference, a lambda, or a wrapper around one). Nothing with C linkage
>     is a candidate, and neither is anything exported while an exports
>     manifest is being written.
>   - **Roots** are every other item. The walk follows every `Binding` a
>     node names, plus the links the emitter may spell in a binding's place
>     (`closure_fn_binding`, `source_fn_def`, `widen_fn_alias`,
>     `deferred_init`, ...). Instance method bodies are walked too.
>   - **It lists every expression kind**, with no `default`, so `-Wswitch`
>     names a kind added later. A kind the walk does not recognize at run
>     time makes it give up and prune nothing.
>   - The removed items take their C functions, forward declarations, fat
>     boxes and startup initializers with them, because the emitter makes all
>     of those from the items it is given.
>   - `TUR_NO_SRFI_PRUNE=1` turns it off, and `TUR_SRFI_PRUNE_DEBUG=1` says
>     what it kept and what reached each one.
>   - `tests/check-r7rs-srfi-prune.sh` (ctest `tur_r7rs_srfi_prune`) is the
>     fixture this stage asked for. `run-r7rs-import.sh` adds a library that
>     uses SRFI 1 beside a program that uses other parts of it, procedures
>     passed as values, and a library-only import.
> - **The pass first kept 191 of SRFI 1's definitions alive** in a program
>   that called none of them. The Scheme lowering defers a `define`'s
>   initializer that comes after the program's first top-level expression,
>   turning it into a `set!` in `main` (R7RS 5.1's order). It applied that
>   to the SRFI's own defines too, since they are lowered in place beside the
>   program. So `(define first car)` became a `set!` in `main`, which named
>   `first`, and everything its initializer named. Only the program's own
>   defines are deferred now. A library's are initialized in the library's
>   order, before the program runs, as a `define-library`'s always were.
> - **The measurement**, repeated as S0 did it (Debug `tur`, 4 cores, idle,
>   median of three; `cc` timed through a `CC` wrapper):
>
>   | Program | `tur build` | `cc` | C lines | C functions |
>   |---|---|---|---|---|
>   | `p0`: `(write 1)` | 3.42 s | 2.53 s | 28,855 | 1,625 |
>   | `p1`: `p0` importing `(srfi 1)`, none of it called | 3.50 s | 2.59 s | 28,855 | 1,625 |
>   | `p1` with the pass off | 4.91 s | -- | 33,929 | 1,862 |
>   | `p2`: `p1` calling `fold` | 4.21 s | 3.16 s | 29,400 | 1,651 |
>   | `p2` with the pass off | 5.01 s | -- | -- | -- |
>   | `p4`: `p0` defining chibi's `fold`, `any`, `every`, `map-onto` itself | 4.06 s | 3.02 s | 29,400 | 1,651 |
>
>   An unused import now costs nothing: `p1`'s C is `p0`'s, up to the
>   numbering of lifted lambdas. `p2` is +0.8 s over `p0`, not the +0.2 s S0
>   estimated from a one-line `fold` (`p3`). That is chibi's `fold`, not the
>   splice. The n-ary branch calls `every`, `map`, `apply` and `map-onto`,
>   and `p4`, the same code written in the program, emits the same 29,400
>   lines and builds in the same time.
> - **SRFI 1 is chibi's** `(else ...)` branch and its nine included files,
>   as written, in `stdlib/srfi/1.scm`. The names it shares with
>   `(scheme base)` and `(scheme cxr)` are re-exported (D5). R7RS's `append`
>   was two-argument, so it became variadic, as R7RS 6.4 says, since chibi's
>   code calls it with one argument and with three.
> - **chibi's suite** is `tests/r7rs/srfi/1/tests.scm`: 157 of the 161 tests
>   the runner counts pass on both back ends. The floor is 157. The four
>   failures:
>   - `(car '())`, `(cdr '())` and `(every odd? '(1 3 . x))` under
>     `test-error` are the uncatchable panics of
>     docs/reported/r7rs-type-errors-are-uncatchable-panics.md;
>   - `(set-car! (g) 3)` on a quoted constant is not an error here. R7RS
>     says mutating a literal "is an error", but it does not require the
>     implementation to signal one; chibi's literals are immutable.
>
>   The runner now finds a form that fails the C build by bisection, fails
>   that form alone, and carries on. It also no longer counts a test that
>   is commented out inside a form.
> - **A capture bug the suite found**, in plain Turmeric as well: a lambda
>   that calls a let-bound lambda did not capture it. `(let [g (fn [] 1)]
>   ((fn [] (g))))` named an undeclared `g` in the C. That was so as far
>   back as v0.41.0. A lambda that captures nothing is held in a local as a
>   C function pointer. `collect_free_vars` counted a call through a local
>   as a capture only for a few kinds of binding (a parameter, a closure, a
>   letrec member, ...), and a plain let-bound one was not among them. It
>   is now any function-typed binding that is not global. The env fill then
>   stores the pointer through `intptr_t` into the carrier field; a plain
>   assignment was a `-Wint-conversion`, an error under clang. Pinned by
>   `tests/fixtures/closure-calls-let-bound-lambda` and
>   `r7rs-guard-calls-local-lambda`.
> - **Fixtures:** `r7rs-srfi-1` pins the edge cases above on both back
>   ends. `errors/r7rs-srfi-not-yet` and `r7rs-srfi-cond-expand` move their
>   "not yet" pin from SRFI 1 to SRFI 69.
> - **Reported, not fixed (since fixed, 2026-09-27):**
>   docs/archive/r7rs-raise-musttail-fails-under-clang-x86-64.md. Under
>   clang on x86-64, any program that raises failed to build, the SRFI
>   suites included. CI compiles with gcc on Linux and with clang on arm64,
>   so it did not see it.

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

> **What shipped (2026-09-27).** Everything above.
>
> - **The primitives** are in the prelude's new hashing section
>   (stdlib/r7rs/prelude.tur), in Turmeric, so both back ends run the same
>   code. Only two are C, each with an interpreter twin in
>   src/turi/interpreter_natives.c:
>   - `r7rs-identity-word__`: a value's carrier word, the payload
>     `r7rs-same-ref__` already compares;
>   - `r7rs-cstr-hash__`: FNV-1a over a string's UTF-8 bytes.
>
>   Every hash is a non-negative fixnum below 2^30.
>   - `hash` (`r7rs-equal-hash__`) walks what `equal?` walks: pairs,
>     vectors, bytevectors and strings. It reads at most 16 elements of a
>     list or vector and 64 bytes of a bytevector, three levels down, so it
>     is quick on a big key and terminates on a cycle, as `equal?` does.
>   - `hash-by-identity` (`r7rs-eqv-hash__`) hashes by value what `eqv?`
>     compares by value: numbers (a bignum by its digits, a ratio and a
>     complex by their parts), string literals, symbols and characters.
>     `'()` and the eof object are constants, and anything else is hashed
>     by address, which is stable because the collector does not move.
>     `eq?` is `eqv?` here, so one hash serves both.
>   - An integral float hashes as the exact integer does, so a table keyed
>     by `=` finds 2 from 2.0, and 0.0 and -0.0 land together, as `eqv?`
>     has them.
>   - `string-ci-hash` folds an ASCII string as it hashes, and folds any
>     other with `string-foldcase` first.
>   - No measurement was needed to choose Scheme over one C walk: a C walk
>     would have needed a second copy for the interpreter's values.
> - **The table** is `stdlib/srfi/69.scm`, written for Turmeric (D6). It is
>   a record over a vector of association-list buckets, doubled past two
>   entries a bucket. All 24 names are exported, with the hash functions'
>   optional bound.
> - **The default hash function.** The reference picks one by comparing the
>   equivalence with `eq?`, `string=?` and the rest by `eq?`, which a
>   standard procedure does not pass here
>   (docs/reported/r7rs-prelude-procedures-lose-identity.md).
>   `(make-hash-table string-ci=?)` would then have hashed case-sensitively.
>   The default instead hashes a string key case-folded and anything else
>   with `hash`. That is right for all five standard equivalences; case
>   variants merely share a bucket.
> - **chibi's suite** is `tests/r7rs/srfi/69/tests.scm`: 84 of 84 on both
>   back ends, and the floor is 84.
>   - It leaves out one chibi-only test (`make-exception`,
>     `exception-kind`).
>   - The runner gains `test-not` and chibi's `test-equal`.
>   - A suite directory may name more imports in an `imports` file; 69's
>     suite uses SRFI 1's `lset=`.
> - **Cost.** An unused `(import (srfi 69))` adds the record's struct and
>   constructor, 22 lines that nothing calls; the pruning pass drops the
>   rest. A program using a table adds about 740 lines.
> - **Fixtures:**
>   - `r7rs-srfi-69` pins the default hash under each standard equivalence,
>     `hash`/`equal?` agreement across representations, a circular key, a
>     `=`-keyed table, and growth, deletion and the whole-table procedures
>     on 500 entries;
>   - it passes under `TUR_GC_TORTURE` in run-r7rs-gc.sh and under the
>     sanitizers;
>   - `errors/r7rs-srfi-not-yet` and `r7rs-srfi-cond-expand` move their
>     "not yet" pin to SRFI 13.

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

> **What shipped (2026-09-27).** Everything above, both SRFIs written for
> Turmeric (D6).
>
> - **The Unicode data.** `tools/gen-r7rs-unicode.py` gains the General
>   Category sets from the UnicodeData.txt it already read, so
>   `tools/fetch-ucd.sh` fetches nothing new: title-case (Lt), punctuation
>   (P\*), symbol (S\*), graphic (L\* N\* M\* S\* P\*) and blank (Zs and
>   U+0009). They are SRFI 14's 2019 CharsetDefs note's definitions, which
>   bring its Java-1.0-era tables to current Unicode. One accessor,
>   `r7rs-uc-run__`, hands Scheme the strided runs of any standard set; it
>   has an interpreter twin, and tests/check-r7rs-unicode-sync.sh covers the
>   new C as it does the old. The generator also gains the titlecase
>   exceptions (op 3 of `r7rs-uc-map__`), which `string-titlecase` needs:
>   U+01C6 titlecases to U+01C5 but uppercases to U+01C4, and Georgian
>   Mkhedruli titlecases to itself.
> - **SRFI 14** (`stdlib/srfi/14.scm`) is a record around an inversion list,
>   a vector of code points. Membership is a binary search, and union,
>   intersection, difference and xor are one merge. A set never changes once
>   made, so the linear-update `!` procedures are the pure ones. Each
>   standard set is built from the tables the first time a program uses it.
>   `char-set:full` is every Unicode scalar value, the surrogates left out.
>   - chibi's suite (Shivers' regression tests) is
>     `tests/r7rs/srfi/14/tests.scm`: 72 of 72 on both back ends.
> - **SRFI 13** (`stdlib/srfi/13.scm`) reads each string argument once, as
>   a vector of characters, and returns fresh mutable strings.
>   - `string-map` and `string-for-each` are its own, and conflict with
>     `(scheme base)`, as D5 said.
>   - The other shared names are R7RS's procedures. `string-upcase` and
>     `string-downcase` became compatible extensions: the prelude's take
>     SRFI 13's optional range. They keep R7RS's full case mapping, where
>     SRFI 13 asks for the one-to-one mapping; the in-place `!` forms use
>     the one-to-one mapping.
>   - `string-filter` and `string-delete` accept the criterion first (the
>     SRFI) or the string first (its drafts, which Guile and Gauche kept).
>   - `string-concatenate` does not use `apply`, which stops at eight
>     arguments here.
>   - SRFI 13 has no suite of its own and chibi has no SRFI 13. The suite is
>     Gauche's (BSD-3), as Larceny carries it, `tests/r7rs/srfi/13/tests.scm`:
>     163 of 163 on both back ends. Larceny's file also holds Guile's suite,
>     which is GPL and is not taken.
> - **An SRFI file may import another SRFI.** `(import (srfi 14))` in 13's
>   define-library spells 14's exports onto their `srfi14--` names and
>   registers its macros. The load expander already spliced the file, so
>   importing (srfi 13) alone brings 14 along.
> - **D5's check was fixed.** It had fired whenever the unit imported
>   `(scheme base)` at all, so its own suggested fix,
>   `(except (scheme base) string-map)`, did not clear it. It now asks
>   whether a `(scheme base)` import set makes R7RS's name visible under its
>   own spelling; `except`, `rename`, `prefix` and an `only` that leaves the
>   name out all clear it. Two resolution bugs on the same paths were fixed
>   with it: a `rename` of a standard name that another import set
>   `except`s, and a `prefix`ed standard name that an SRFI also binds, both
>   resolved to the wrong procedure. The `(scheme base)` import sets stand
>   for every standard library, since the lowering keeps no per-library
>   name lists. That is exact for 13's two names.
> - **The suite runner** gains `--base-except`, and a suite directory a
>   `base-except` file, for 13's suite. `test-cs` counts as a test.
> - **Cost.** An unused `(import (srfi 13))` or `(import (srfi 14))` adds
>   143 emitted lines, the standard sets' records; the pruning pass drops the
>   rest. A program that tests `char-set:letter` adds about 890 lines, and
>   one that uses `string-index`, `string-trim` and `string-join` about
>   2,200.
> - **Fixtures:**
>   - `r7rs-srfi-14`: the standard sets on ASCII and beyond, their sizes
>     against the UCD, the surrogate hole, and cursors;
>   - `r7rs-srfi-13`: fresh results, ranges, the hashes, both
>     `string-filter` orders, titlecase of the digraphs and Georgian, and
>     the low-level and KMP procedures;
>   - `errors/r7rs-srfi-13-conflict` pins D5's error for the first
>     incompatible row, and `r7rs-srfi-conflict-fixes` every fix it names;
>   - the three that run a program pass under `TUR_GC_TORTURE` in
>     run-r7rs-gc.sh and under the sanitizers;
>   - `errors/r7rs-srfi-not-yet` and `r7rs-srfi-cond-expand` move their
>     "not yet" pin to SRFI 19, an S8 row.

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

> **What shipped (2026-09-27).** Everything above. Three compiler bugs the
> ports hit are fixed with them, and one gap on the uncatchable-panics report
> is closed.
>
> - **SRFI 48** (`stdlib/srfi/48.scm`) is the reference implementation (D6:
>   portable, MIT), as the SRFI's repository carries it with Hamayama's 2017
>   `~F` fixes. The adaptations are marked in the file:
>   - a call without a port no longer goes through `apply`, which stops at
>     eight arguments here;
>   - the format string is copied once, so `string-ref` on it is constant
>     time (2.5);
>   - R7RS's `inexact`/`exact` and `write-shared` stand in for their R5RS
>     and SRFI 38 names.
>
>   The repository's test file is the suite, `tests/r7rs/srfi/48/tests.scm`:
>   183 passed and 14 settled of 197 on both back ends. The 14 assume
>   Gauche's `number->string`, which switches to exponent notation sooner
>   (`3.2e11`, `-3e-4`); here those print in full. The SRFI leaves that point
>   to the implementation. Each is a SETTLED entry in the runner, and the
>   runner checks at run time that the spelling still holds.
> - **SRFI 28** is SRFI 48's `format`, re-exported. Importing both binds it
>   once. An SRFI library may now be only a re-export of an SRFI it imports,
>   with no body; tests/check-r7rs-srfi-sync.sh accepts that.
> - **SRFI 64** (`stdlib/srfi/64.scm`) is Taylan Kammer's R7RS
>   implementation from the repository, its four libraries flattened into
>   one. Changes, marked in the file:
>   - `test-runner-factory` and `test-runner-current` are globals behind
>     procedures; the original sets parameter objects by calling them;
>   - the default runner writes no log file;
>   - after the summary, the default runner's outermost `test-end` exits
>     with status 1 when a test failed or passed unexpectedly. That is the
>     `tur test` integration: a failing suite fails the file, and a clean one
>     returns so the program goes on;
>   - `test-error` takes `#t` or a predicate (SRFI 35's condition types come
>     in S7);
>   - `test-read-eval-string` is syntax. It spells `eval` and the reader
>     where it is used, so only a program that calls it pays for
>     `(scheme eval)` (the interpreter, +3 s and ~1.5 MB) or `(scheme read)`;
>   - no source locations (no syntax-case), so a failure prints its form.
>
>   The SRFI's meta-suite is `tests/r7rs/srfi/64/tests.scm`, 53 of 53 on
>   both back ends. It is itself an SRFI 64 program, so the runner gains
>   `--self-hosted` (a `self-hosted` marker in the suite directory): it runs
>   the file whole and reads the summary. `tur init --r7rs` scaffolds an SRFI
>   64 test in both shapes, and run-init-r7rs.sh checks that a failing one
>   fails `tur test`.
> - **SRFI 78** (`stdlib/srfi/78.scm`) is the reference `check.scm`, less
>   `check-ec`. Its mode and counters start initialized instead of being set
>   by load-time calls.
> - **Fixed on the way:**
>   - *An `if` joining a capturing closure and a thin function*
>     (`((if c (lambda () c) (lambda () 0)))`): the thin arm was stored raw
>     where the join expects a fat box, and calling it read code as an env.
>     The join now boxes the thin arm (src/compiler/elab_forms.c). SRFI 48's
>     `format` with a port crashed on it. Pinned by
>     `tests/fixtures/if-joins-closure-and-thin-fn`.
>   - *A `letrec` member calling an earlier sibling that is a closure* was
>     not captured, and cc rejected the lifted body. SRFI 64's simple runner
>     has two internal defines of that shape. Now captured
>     (src/compiler/elab_core.c; `letrec-sibling-closure-capture`). Two
>     capturing members that call each other were resolved 2026-09-28:
>     docs/archive/letrec-mutual-recursion-between-capturing-closures.md.
>   - *A top-level `(define f (case-lambda ...))` whose clause calls `f`* was
>     "unknown function f"; it is a defn now, as a lambda define is
>     (`r7rs-case-lambda-define-recurs`).
>   - *`vector-ref` and `vector-set!` out of range* raise an error object
>     instead of panicking. That is what `test-error` tests, in the
>     meta-suite and in practice (`r7rs-vector-index-error`). The rest of
>     docs/reported/r7rs-type-errors-are-uncatchable-panics.md stays open.
>     (Resolved 2026-09-27: docs/archive/r7rs-type-errors-are-uncatchable-panics.md.)
> - **Cost.** An unused import adds 14 emitted lines for (srfi 48), 4 for
>   (srfi 78) and 320 for (srfi 64), whose `(scheme process-context)` is
>   most of that. A one-test SRFI 64 file adds about 5,400 lines and 2 s of
>   build over an empty program. One `format` call adds about 2,000 lines.
> - **Fixtures:** `r7rs-srfi-28`, `r7rs-srfi-48`, `r7rs-srfi-64`,
>   `r7rs-srfi-64-failing` (`expected.exit` 1), `r7rs-srfi-64-read-eval` and
>   `r7rs-srfi-78`, plus the three bug fixtures above. All pass on both back
>   ends, under `TUR_GC_TORTURE` and under the sanitizers.

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

> **What shipped (2026-09-27).** Everything above: seven SRFIs and
> `check-ec`, each with its suite on both back ends and a fixture. Suites
> and floors (both back ends pass the same count):
>
> | SRFI | File | From | Suite | Passes |
> |---|---|---|---|---|
> | 4 | `4.scm` | written for Turmeric | cowan's `shared-tests.scm`, each type written out, plus range checks | 270/270 |
> | 27 | `27.scm` | the reference MRG32k3a | the reference's `conftest.scm` and chibi's histograms | 36/36 |
> | 35 | `35.scm` | the reference | chibi's, plus the error-object bridge | 69/69 |
> | 41 | `41.scm` | chibi's adaptation of the reference | chibi's | 175 interp, 174 compiled, of 187 |
> | 42 | `42.scm` | the reference `ec.scm`, unchanged | the reference's `examples.scm` | 163/163 |
> | 60 | `60.scm` | the document's (SLIB `logical.scm`) | the document's examples, plus bignums and a fast-path cross-check | 94/94 |
> | 66 | `66.scm` | written for Turmeric | the document, procedure by procedure | 40/40 |
>
> - **SRFI 42**: its `:` is a lone colon, which the reader now reads as a
>   symbol in a Scheme source (src/compiler/reader.c); `(: i 3)` was a
>   type-annotation error. No expander limit was hit. A qualifier the
>   program writes (`(:list x xs)` in a `check-ec`) expands in the program's
>   scope, so a program using `check-ec` imports `(srfi 42)` too.
> - **SRFI 41**: two lowering gaps, fixed. A record type with no fields
>   (the null stream's) gets a hidden field, and a top-level define whose
>   init names itself (`(define nats (stream-cons 0 (stream-map add1
>   nats)))`) is pre-declared like a forward reference. The 13 suite
>   failures are `test-error` cases: type errors that panic
>   (docs/reported/r7rs-type-errors-are-uncatchable-panics.md) and calls with
>   too few arguments, which return a procedure instead of raising
>   (docs/reported/r7rs-too-few-arguments-returns-a-procedure.md). Both were
>   fixed 2026-09-27 (docs/archive/), and the suite passes in full (187).
> - **SRFI 35** decides section 7's question 4 (above). It carries local
>   copies of the five SRFI 1 procedures it uses, so importing it (and SRFI
>   64, which imports it for `test-error`'s condition types) does not splice
>   SRFI 1: that saved 0.6 s of every SRFI 64 build.
> - **SRFI 27**'s histograms draw 1,000 numbers each, not chibi's 10,000:
>   the interpreter spends about 3 ms on a bignum draw, and the seed is
>   fixed. The reference's last check (a sum over 10^7 reals) is left out
>   for time; its `pseudo-randomize!` state check pins the same arithmetic.
>   `randomize!` seeds from `current-jiffy`.
> - **SRFI 60** is the plan's split: two fixnums go to four new prelude
>   helpers over Turmeric's bit operations (`r7rs-fx-and__`, `-ior__`,
>   `-xor__`, `-shr__`), plain Turmeric, so they need no interpreter twin; a
>   bignum is taken 30 bits at a time by `floor-quotient` and `modulo`, where
>   the reference went 4 bits at a time through two tables.
> - **SRFI 4** is not Appendix C's cowan library, which sits on a 1,000-line
>   R6RS bytevector layer. The nine non-u8 types are records over Scheme
>   vectors with range checks, and `u8vector` is SRFI 66's, re-exported, so
>   importing both SRFIs binds each name once. Cowan's shared tests are the
>   suite.
> - **Fixed on the way:**
>   - *`set!` on a top-level procedure's parameter* was "'x' is immutable":
>     the lambda path rebinds such a parameter as a mutable local, the
>     `(define (f x) ...)` path did not. SRFI 60's `rotate-bit-field` found
>     it. And a set rest parameter was rebound twice, whose second binding
>     failed to build (`r7rs-toplevel-define-sets-its-parameter`).
>   - *Bytevector bytes and indices, and every sequence range*, raise an
>     error object: `bytevector-u8-set!` stored 256 silently, `(bytevector
>     300)` and a bad `substring`/`vector->list`/`bytevector-copy!` range
>     panicked (`r7rs-bytevector-range-errors`).
> - **Order of evaluation.** The two back ends evaluate a call's arguments
>   in different orders, which R7RS allows; `(list (rand) (rand))` differs
>   between them. The fixtures sequence their draws.
> - **Cost.** Emitted lines an unused import adds to a one-line program: 0
>   for (srfi 66), 18 for (srfi 4), 74 for (srfi 41), 234 for (srfi 35), 652
>   for (srfi 60), 2,076 for (srfi 27) and 5,500 for (srfi 42). The last two
>   keep top-level state the pruner cannot drop: SRFI 27's
>   `default-random-source` and SRFI 42's `:-dispatch` table, which names
>   every generator. (srfi 64) grew from 320 to 554, by SRFI 35's types.
> - **Fixtures:** `r7rs-srfi-4`, `-27`, `-35`, `-41`, `-42`, `-60`, `-66`,
>   `r7rs-srfi-78` (now with `check-ec`), `r7rs-record-type-without-fields`,
>   `r7rs-toplevel-define-refers-to-itself`,
>   `r7rs-toplevel-define-sets-its-parameter` and
>   `r7rs-bytevector-range-errors`.

### S8 -- the long tail (on demand)

These are not scheduled, and a row flips when someone asks for it: 5, 7, 19,
25, 29, 43, 54, 57, 59, 63, 67, 71, 74, 86. Each is a file, a fixture, and a
row in the table.

---

## 7. Open questions

1. **SRFI 62: an error (Racket) or a no-op?** **Decided 2026-09-26: an
   error**, as in Racket, per the ask's "if Racket does same"; the message
   says `#;` needs no import. Shipped in S1
   (`tests/fixtures/errors/r7rs-srfi-62-no-library`).
2. **`(srfi :1)` and `(srfi 1 lists)`.** SRFI 97's R6RS-era spellings, which
   some portable code uses. **Decided 2026-09-26: not accepted**, as in
   Racket's R7RS. Both are refused on both back ends with "an SRFI is named
   by its number, e.g. (srfi 1)", which is the fix. Revisit when a port
   needs them.
3. **Turmeric importing an SRFI.** `(import srfi/1 ...)` from a `.tur` file
   would need the SRFI libraries as modules, which D3 avoids. **Decided
   2026-09-26: out of scope.** A Turmeric program has the typed stdlib, and a
   Scheme library can wrap an SRFI for it.
4. **SRFI 35 and R7RS error objects.** Should `(condition-has-type? e &error)`
   hold for an `error` object, and `error-object?` for an SRFI 35 `&error`
   condition? Chibi and Gauche differ. **Decided 2026-09-27 (S7): the first
   yes, the second no.** An R7RS error object is a condition of types
   `&error` and `&message`, its message `error-object-message`'s, so a
   handler written against SRFI 35 (`(error? e)`, `(condition-message e)`)
   works on what `error` and the primitives raise. The reverse would need
   the prelude's `error-object?` to know SRFI 35's record type, and an SRFI
   35 condition has no irritants to give `error-object-irritants`; code that
   raises SRFI 35 conditions tests them with SRFI 35's predicates. The whole
   bridge is in `stdlib/srfi/35.scm` (`condition?` and the type-field alist
   accept an error object), and `tests/fixtures/r7rs-srfi-35` pins it.
5. **SRFI 69's `hash`.** The SRFI's `hash` takes an optional bound; R7RS-era
   code often expects SRFI 128's `default-hash`. Keep 69's names exact, and let
   125/128 (Section 5) add theirs over the same primitives. **Decided
   2026-09-27 (S4): as proposed.** 69 exports its own names only, and the
   prelude's `r7rs-equal-hash__`, `r7rs-eqv-hash__`, `r7rs-string-hash__`
   and `r7rs-string-ci-hash__` are what 125 and 128 will export under
   theirs.

---

## 8. Risks

- **Build time.** Measured in S0: an unpruned SRFI 1 splice costs every
  importer about 1.6 s on a 3.5 s build. D3's shape stays; the pruning pass S3
  builds first is the mitigation. A big SRFI landing before that pass does is
  the risk.
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

## Appendix C -- S0 inventory

Gathered 2026-09-26 from each SRFI's repository (`github.com/scheme-requests-for-implementation/srfi-<N>`)
and from chibi-scheme's `lib/srfi/` (BSD-3, the licence already vendored as
`tests/r7rs/CHIBI-COPYING`). Every SRFI document carries the MIT licence,
whose text names "this software and associated documentation files", except
SRFI 5, which carries the pre-2019 SRFI notice (copying and "derivative works
that ... assist in its implementation" are allowed without restriction). So a
reference file with no header of its own is covered by its document's MIT
notice. "Document" below means the implementation is in the SRFI's HTML.
chibi's test counts are the `(test ...)` forms in its `test.sld`.

Rows that need no implementation: 0, 62 and 105 (no library); 6, 9, 11, 16,
23, 30, 34, 39, 87 and 98 (built in); 38 and 45 (alias). chibi has test suites
for 16 (a `test.sld`) and 38, so S1's "built in" can be checked against them.
40 is not planned.

| SRFI | Stage | Start from | Licence | Test suite |
|---|---|---|---|---|
| 1 | S3 | chibi `1.sld` + `1/*.scm` (R7RS, 492 lines; ran unmodified in S0). Reference: `srfi-1-reference.scm` (Shivers, 1,596 lines; `:optional`, `let-optionals`, `check-arg`) | chibi BSD-3; reference: "do as you please ... do not remove this copyright notice", SPDX MIT | chibi (156) |
| 2 | S2 | document; chibi `2.sld` | MIT; chibi BSD-3 | chibi (31) |
| 4 | S7 | repo `contrib/cowan/` (R6RS and R7RS libraries over bytevectors) | no header: document MIT | repo `contrib/cowan/all-tests`, `r6rs/shared-tests.scm` |
| 5 | S8 | document | pre-2019 SRFI notice (see above) | none |
| 7 | S8 | document | MIT | none |
| 8 | S2 | document; chibi `8.sld` | MIT; chibi BSD-3 | none |
| 13 | S5 | write our own (D6). Reference: `srfi-13.scm` (Shivers; MIT Scheme and scsh code) | reference: MIT Scheme licence (clause 2 asks users to return improvements to MIT, clause 3 asks for acknowledgement) plus scsh's BSD | none |
| 14 | S5 | chibi `14.sld` | chibi BSD-3; reference `srfi-14.scm`: MIT Scheme licence, as 13 | chibi (6); repo `srfi-14-tests.scm` |
| 17 | S2 | document (the repo's `srfi-17-twobit.scm` is Larceny-specific) | MIT | none |
| 19 | S8 | `srfi-19.scm` (I/NET; a Gauche variant alongside) | MIT | repo `srfi-19-test-suite.scm` |
| 25 | S8 | `array.scm` with the `op-*`/`ix-*` files (Piitulainen) | no header: document MIT | repo `test.scm` |
| 26 | S2 | `cut.scm` (Egner); chibi `26.sld` | public domain; chibi BSD-3 | repo `check.scm`; chibi (2) |
| 27 | S7 | `srfi-27-reference/mrg32k3a.scm` + `mrg32k3a-a.scm` (Egner; all Scheme). chibi's is C (`rand.c`) | no header: document MIT | repo `conftest.scm`; chibi (2) |
| 28 | S6 | `srfi/28.sld` (Miller; already an R7RS library) | SPDX MIT | none |
| 29 | S8 | document | MIT | none |
| 31 | S2 | document (a `syntax-rules` definition) | MIT | none |
| 35 | S7 | document; chibi `35.sld` | MIT; chibi BSD-3 | chibi (38) |
| 41 | S7 | `streams.ss` + `primitive.ss` + `derived.ss` (Bewig; R6RS libraries) | MIT | repo `r5rs-test.ss`, `r6rs-test.ss`; chibi (111) |
| 42 | S7 | `ec.scm` (Egner) | no header: document MIT | repo `examples.scm` (171 self-checking `my-check` forms) |
| 43 | S8 | `vector-lib.scm` (Campbell) | public domain | none |
| 48 | S6 | document | MIT | repo `test/` (Guile and Racket variants) |
| 54 | S8 | document | MIT | none |
| 57 | S8 | document | MIT | none |
| 59 | S8 | document | MIT | none |
| 60 | S7 | document | MIT | none |
| 61 | S2 | document | MIT | none |
| 63 | S8 | document | MIT | none |
| 64 | S6 | repo `contrib/taylan.kammer/` (R7RS); chibi `64.sld` | MIT; chibi BSD-3 | repo `srfi-64-test.scm` |
| 66 | S7 | document | MIT | none |
| 67 | S8 | `implementation/compare.scm` (Egner, Sogaard) | MIT | repo `implementation/examples.scm` |
| 69 | S4 | write our own (D6, S4). Document has one; chibi's is C (`hash.c`) | MIT; chibi BSD-3 | chibi (34) |
| 71 | S8 | `letvalues.scm` (Egner) | no header: document MIT | none |
| 74 | S8 | `blob.scm` (Sperber) | MIT | none |
| 78 | S6 | `check.scm` (Egner; the SRFI is itself a test library) | no header: document MIT | repo `examples.scm` |
| 86 | S8 | document | MIT | none |

Two notes for the stages:

- A port keeps its copyright header and gets a line in `stdlib/srfi/COPYING`
  (D6). A chibi port's line points at `tests/r7rs/CHIBI-COPYING`.
- A chibi `test.sld` is a `(srfi N test)` library whose `run-tests` holds
  the tests, written against `(chibi test)`. The conformance runner already
  defines a `(chibi test)`-compatible `test` family, but it takes a flat file
  of top-level forms (`tests/r7rs/chibi-r7rs-tests.scm`). So porting a suite
  means lifting `run-tests`' body out into such a file under
  `tests/r7rs/srfi/<N>/`, with no change to the tests themselves.

---

## See also

- [r7rs-lang-plan.md](r7rs-lang-plan.md) -- the dialect this extends; D9 (the
  library system) and R7 (the on-demand library mechanism S1 reuses).
- [docs/guides/r7rs-guide.md](../guides/r7rs-guide.md) -- where the support
  table goes.
- [docs/guides/experimental-flags-guide.md](../guides/experimental-flags-guide.md)
  -- the rule D8 reasons about.
