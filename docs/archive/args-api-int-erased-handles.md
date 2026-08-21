# stdlib/args.tur types its handles and option defaults as bare :int

**Severity: medium** -- a "No Lazy `:int`" violation in first-party stdlib
API: the spec/result handles and the option-default slot are `:int`, and the
default is a cstr smuggled through `:int`, forcing every caller to define a
reinterpret helper. Found in the 2026-08-20 docs audit.

## Repro

`(args/spec-option spec "--count" "int" "1")` is a type error;
`tests/fixtures/args-defaults` works around it with a local
`(defn cstr->int [s : cstr] : int ...)`.

## Root cause

stdlib/args.tur:35-129 (`spec : int`, `dflt : int`), :379+ (`result : int`).

## Fix direction

`defopaque ArgSpec`/`ArgResult` newtypes; `dflt : (Option cstr)`.

## Guides to update when fixed

- docs/guides/cli-args-guide.md (Building-a-Spec note, the cstr->int helper,
  API table)

## Resolution (2026-08-21)

Fixed along the filed fix direction. `stdlib/args.tur` now declares
`(defopaque ArgSpec :int)` and `(defopaque ArgResult :int)`, and every one of
the 18 public entry points names one of them instead of `:int`. Passing a spec
where a result belongs is now `TUR-E0001: expected ArgResult, got ArgSpec`
rather than a silently wrong pointer.

The option default is `dflt : (Option cstr)` -- `(some "1")` for a default,
`(none)` for a required option. The old `0`-means-required convention is now a
type error too. `args/sub-result` returns `(Option ArgResult)` for the same
reason: its `0` slot was an optional handle spelled as an integer sentinel.

Two shape notes for anyone touching this file:

- **An inline-C body cannot take `(Option cstr)` directly.** On the by-value
  HKT path it lowers to `tur_adt_Option__cstr { bool is_some; const char *value; }`,
  not the `int64_t` carrier the `tur_is_some` / `tur_opt_value` preamble
  helpers accept -- `tur_is_some(dflt)` fails to compile with
  `incompatible type for argument 1`. The option is peeled in a pure-Turmeric
  body (`.is-some` / `.value`, the same idiom `unwrap-or` uses) which then
  calls a `cstr`-taking inline-C helper. Same for the returning side:
  `args/-sub-result-raw` returns the raw slot and the public wrapper mints the
  `(some ...)` / `(none)`.
- **Docstring blocks bind to the NEXT definition**, so an internal helper
  inserted between a `;;;` block and its public `defn` silently steals the
  docstring -- `gendocs.py` emitted `args/-no-default` carrying
  `args/spec-option`'s text. The helpers sit above the public block now.

Deliberately unchanged: `args/parse`'s `argv` and `args/positional`'s return
stay `:int`. Both are the `*args*` cons list, and the elaborator pre-declares
`*args*` itself as a global `:int` binding (`elab_core.c:2195`) -- typing this
one seam differently from the global it carries would not buy checking.
Also unchanged: `args/get-str` / `args/subcommand` / `args/error-msg` return a
NULL `:cstr` when absent. That is a cstr sentinel, not an `:int` type-eraser,
and retyping it is an API break for every reader of a string option; worth
doing when the `cstr`-vs-`(Option cstr)` convention is settled stdlib-wide
rather than in this one module.

Verified: `bash tests/run.sh` is 2676 passed, 0 failed, including the four
`args-*` fixtures. `args-defaults` no longer needs its local `cstr->int`
reinterpret helper -- the workaround the report cited is deleted.

## Guides updated

- `docs/guides/cli-args-guide.md` -- Building-a-Spec prose (the `cstr->int`
  helper is gone from both the s-expr and sweet-exp samples), the subcommand
  example's `args/sub-result` uses, and every row of the API table.
- `README.md` -- the CLI-argument-parsing example called a nonexistent
  `args/get` and passed a bare cstr default; it now uses `args/get-str`,
  `(some "out.txt")`, and frees what it allocates.
