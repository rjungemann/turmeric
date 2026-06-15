# Spice defn return type `(Result A B)` rejected as kind `*`

**Status:** Fixed (2026-06-14)
**Severity:** Expressiveness hole -- blocks the S3 (`result<T,E>` / `option<T>`
collapsed to `:int`/`ptr<void>`) phase of the spices `:int`-stand-in audit.
Until this is fixed, spices cannot spell `(Result Foo cstr)` as a `defn`
return type, and the only workable carrier-result idiom is the
layout-compatible `ptr<void>` inline-C `__rr` struct that the audit calls
out as the S3 defect in the first place.
**Discovered:** 2026-06-14, while landing the regex S2 fix
(`docs/reported/spices-int-stand-in-audit-2026-06-14.md`,
`tur-regex` v0.2.0). The S2 (handle opaques) part landed cleanly; the
S3 part (return as `(Result Regex cstr)`) is what surfaced this.

---

## Summary

In a spice compiled with `tur build --shared`, declaring a `defn` return
type as `(Result Regex cstr)` (or `(Result cstr cstr)`, or any other fully
concrete `Result A B`) is rejected with `TUR-E0012: cannot apply a type of
kind '*' as a type constructor; expected an arrow kind (* -> * or higher)`.
The same syntactic shape works in two places in the codebase:

1. **stdlib/result.tur** itself, e.g. `(defn ok-val [A B] [r : (Result A B)] :A ...)`
   -- this introduces `[A B]` as defn-level tyvars, so `Result` is being
   *applied to tyvars* rather than to concrete types.
2. **json spice `defclass`**, e.g.
   `(defclass Decode [a] (decode [doc val] : (Result a cstr)))`
   -- here `a` is a class-bound tyvar; instances then return through the
   `tur_ok`/`tur_err` runtime carriers.

Neither demonstrates the case the audit's S3 phase needs: a `defn` whose
return type pins both arms to concrete types (e.g.
`(Result Regex cstr)`) so the public API is type-checked at the use site
without forcing every caller to add their own `(:: ... (Result Regex cstr))`
ascription.

## Observed

In `../turmeric-spices/spices/regex/src/regex/regex.tur` (the v0.2.0 work
in progress), the following form...

```turmeric
(defopaque Regex :int)
(defopaque Match :int)

(defn regex-compile [pattern : cstr flags : int] : (Result Regex cstr)
  ```c
  ...
  return tur_ok((int64_t)(intptr_t)w);
  ```)
```

...fails at type-check with:

```
.../regex/regex.tur:42:52: error [TUR-E0012]: kind mismatch (TUR-E0012):
  cannot apply a type of kind '*' as a type constructor; expected an arrow
  kind (* -> * or higher)
42 | (defn regex-compile [pattern : cstr flags : int] : (Result Regex cstr)
   |                                                    ^^^^^^^^^^^^^^^^^^^
```

The same error fires on `(Result Match cstr)` and `(Result cstr cstr)`.

`Result` is registered (a bare `(:: x Result)` would presumably say
"expected 2 args"), but in this position the kind lookup is returning
`*` rather than `* -> * -> *`.

## Expected

A spice that has the stdlib preload should be able to write

```turmeric
(defn regex-compile [pattern : cstr flags : int] : (Result Regex cstr) ...)
```

and have the type checker accept it as "function returning the typed
`Result` with ok-arm `Regex` and err-arm `cstr`", lowering the body's
`tur_ok((int64_t)(intptr_t)w)` to the same int64 carrier value that
`make-struct Result :is-ok true :ok-val w :err-val (default-of cstr)`
produces.

Callers should then get `(ok? r)` / `(ok-val r)` / `(err-val r)` typed at
`Regex` and `cstr` respectively, with no per-call-site ascription needed.

## Hypothesis

The spice compile path preloads the `Result` *carrier helpers* (`tur_ok`,
`tur_err`, `ok?`, `ok-val` operating on the int64 carrier) but not the
full `defstruct Result [A B] ...` kind binding. The interp path
explicitly preloads `result.tur` twice (`src/main.c:10133-10141`),
which is what lets `(Result a cstr)` work inside `defclass` heads --
the class machinery doesn't require the struct to be a real type
constructor in the elaborator's kind table, it just needs the symbol
to resolve. A `defn` return type *does* run a kind check, and that
check is what's failing.

`src/passes/kind_check.c:188` is the emission site of this exact
diagnostic. The interesting question is whether `Result` is even
present in the kind table for the spice compile, or whether it's only
in some carrier-helper table that the kind pass doesn't consult.

I did not chase this past the diagnostic, so the hypothesis above is
not yet verified -- just the most likely shape given how the
interp-path preload is wired.

## Why it matters / why this is a real bug

The S3 phase of the audit
(`docs/reported/spices-int-stand-in-audit-2026-06-14.md`) is the
biggest *ergonomics* win in the whole audit:

> Mechanical -- the bodies already build the underlying
> `result<cstr>` / `option<Response>` shape; just declare the right
> type. Big readability win at handler sites.

If `defn ... : (Result A B)` doesn't work in a spice, the "mechanical"
fix isn't mechanical -- each spice would either have to (a) keep
returning `ptr<void>` and force callers into per-site `(:: ...
(Result A B))` ascription, or (b) wrap every return in a wrapper
that goes through a class instance just so the class's tyvar
machinery is in play. Both defeat the point.

It also means that `tur-regex` v0.2.0 had to stop at S2 (handle
opaques only); the carriers are still `ptr<void>`. Every downstream
S3 fix in any spice is blocked the same way.

## Repro

```sh
cat > /tmp/repro.tur <<'EOF'
(defmodule repro/repro
  (export Foo make-foo)

(defopaque Foo :int)

(defn make-foo [] : (Result Foo cstr)
  ```c
  return tur_ok((int64_t)42);
  ```)

) ;; end defmodule
EOF

/Users/rjungemann/Projects/turmeric/build/tur check /tmp/repro.tur
# => TUR-E0012: cannot apply a type of kind '*' as a type constructor
```

A minimal driver in a real spice tree (where the stdlib preload runs)
reproduces the same way -- see the (reverted) earlier draft of
`../turmeric-spices/spices/regex/src/regex/regex.tur` at git HEAD~0.

## Proposed fix directions

1. **Confirm the hypothesis** -- add a debug print in `kind_check.c:188`
   that dumps `Result`'s recorded kind at the point of failure. If
   the kind table has `Result` at kind `*` (not `* -> * -> *`),
   that's the bug.
2. **Wire the kind binding into the spice/build preload**. The
   carrier helpers are already preloaded; whatever step normally
   registers `defstruct Result [A B] ...` for the kind checker needs
   to run in the spice path too. Likely a missing preload in the
   build/--shared codepath that the interp path already has.
3. **Fixture coverage**: add a fixture
   `tests/fixtures/spice-defn-result-return-concrete/` (or its
   equivalent in this repo's fixture suite) that declares
   `(defn f [] : (Result int cstr) ...)` at module scope (no class,
   no defn-level tyvars) and asserts it type-checks. Today this
   would fail; once the fix lands it should pass.

## Validation of a fix

- The repro in this report typechecks cleanly.
- `tur-regex` v0.2.1 can retype `regex-compile` / `regex-match` /
  `regex-replace` from `ptr<void>` to `(Result Regex cstr)` /
  `(Result Match cstr)` / `(Result cstr cstr)` and the spice still
  builds.
- A consumer of `tur-regex` v0.2.1 can write
  `(ok-val (regex-compile ...))` and have the result typed at
  `Regex` without any `::` ascription.
- The S3 column of the spices `:int` audit becomes unblocked for
  every other affected spice (tourist, httpd, sqlite, postgres, ...).

## Fix (2026-06-14)

Root cause was confirmed exactly as hypothesised: `compile_to_h` /
`compile_to_implementation` (the project-mode entry points) called
`load_project_prelude`, which only loaded `macros.tur` -- so the
defstruct binding for `Result` (and `Option`, `Pair`, `Tuple*`, ...)
never reached the kind table.  The interp / single-file `compile_to_c`
path was unaffected because it explicitly preloads the typed
stdlib.

Landed in this commit:

- `src/main.c:load_project_prelude` -- the prelude now also reads
  `safe.tur`, `hamt.tur`, the typeclass class stubs
  (`Eq`/`Functor`/`Clone`/`Hash`/`Applicative`/`Alternative`/`Monad`
  /`MonadError`/`Bifunctor`), `option.tur`, `result.tur`, `pair.tur`,
  and `tuple.tur`.  Project-mode TUs now see the parametric struct
  bindings at their full `* -> * -> *` (etc.) kinds.
- `src/compiler/emit_module.c:emit_implementation` (struct typedef
  emission) -- wraps each project-mode struct typedef in
  `#ifndef TUR_STRUCT_<Name>_DEFINED ... #endif` so importing two
  module headers that both re-emit `typedef struct Result { ... }
  Result;` no longer produces a `redefinition` error.
- `src/compiler/emit_fns.c:needs_static` and
  `src/compiler/emit_module.c:emit_fn_forward_decls` -- stdlib bindings
  (`is_from_stdlib`) stay `static` in every project-mode TU.  Without
  this, every spice module's .c emits external copies of `ok`,
  `ok-val`, `ok?`, etc., and the linker rejects the duplicates.
- `src/compiler/emit_module.c:emit_header` -- a header forward decl
  for an inline-C-bodied defn now mirrors the same "inline-C with
  TY_APP return -> int64_t carrier" rule that `emit_fn_forward_decls`
  and `emit_fns.c` already used, so the public `.h` prototype agrees
  with the `.c` definition.

Validation:

- The minimal repro at the top of this report now typechecks and
  builds.
- `tur-regex` v0.2.0 retypes `regex-compile` / `regex-match` /
  `regex-replace` from `ptr<void>` to `(Result Regex cstr)` /
  `(Result Match cstr)` / `(Result cstr cstr)` and still builds
  cleanly via `tur build --shared`.
- The full fixture suite is no worse than baseline (1584 passed /
  80 failed both before and after; the 80 failures are the
  pre-existing ones unrelated to this change).

The S3 column of the spices `:int`-stand-in audit
(`docs/reported/spices-int-stand-in-audit-2026-06-14.md`) is now
unblocked.

## Cross-references

- `docs/reported/spices-int-stand-in-audit-2026-06-14.md` -- the
  audit this is blocking the S3 column of. Regex S2 landed; regex
  S3 needs this fix.
- `stdlib/result.tur:13` -- where `Result [A B]` is declared.
- `src/main.c:10133-10141` -- interp-path preload that explicitly
  reloads `result.tur` (the spice/build path needs the equivalent).
- `src/passes/kind_check.c:188` -- the diagnostic emission site.
