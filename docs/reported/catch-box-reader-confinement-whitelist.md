---
status: open
severity: low
discovered: 2026-07-23
area: compiled backend (catch-unwind box escape analysis / deep-free confinement)
---

# `catch-box` deep-free confinement trusts a hardcoded print-family sink list

## Summary

The Leak 2 fix in `catch-unwind-panic-payload-leaks` (resolved 2026-07-23, see
`docs/archive/catch-unwind-panic-payload-leaks.md`) deep-frees a let-bound
caught Result box when it is read through a reader that returns a pointer INTO
the box-owned message (e.g. the inline-C `panic-msg` accessor) *provided* the
reader's result is "confined" -- consumed by a **hardcoded** set of print-family
sinks.

That whitelist is a trusted list, not a checked invariant: it is a
soundness-maintenance footgun, and it needlessly rejects safe user-defined
sinks. This is forward-direction work, not a live bug -- all reported fixtures
and `bash tests/run.sh` are clean, and the residual only ever leaves a
**bounded** per-site leak (never the unbounded loop case, which is fully fixed).

## Detail -- hardcoded non-retaining sink list

`box_reader_result_void_sink` (`src/compiler/emit_core.c:1006`) enumerates
`println`, `print`, `print-show`, ... as the calls that "consume their argument
and never retain a pointer into it." `box_uses_confined`
(`src/compiler/emit_core.c:1040`, consulted at `:1090` / `:1105`) treats a
reader result reaching one of these as safe to free after; the whole check is
`catch_box_binding_reader_confined` (`:1138`), wired into the native
`let_binding_box_freeable` (`src/compiler/emit_expr.c:1421`).

The whitelist is correct **today** only because those runtime functions happen
not to stash a pointer into their `cstr` argument. Nothing enforces it:

- Adding a name that DOES retain (or a future `println` overload that caches)
  silently turns the confinement check into a use-after-free generator: the box
  (and its owned message) is freed at scope exit while the retained alias is
  still live.
- User-defined void sinks (a custom logger, `write-line`, ...) are NOT on the
  list, so an equally-safe `(my-log (panic-msg r))` leaks conservatively.

This is the same opacity the codebase already handles elsewhere with a
**contract** rather than a name list: `^borrow` fn-params, and the inline-C
carve-out in `nonretain_param_mask` inference (`src/compiler/elab_fns.c` --
"an inline-C body can STORE a fn-param invisibly ... never treated as
non-retaining").

## Not a gap: the stackless path

Worth recording so it is not re-investigated: the reader-confined relaxation is
native-path only (the stackless splitter,
`src/compiler/emit_fns.c:1927`, uses the strict scalar-accessor
`catch_box_binding_escapes`), but this is **not** a hole. A caught-box body that
reads through a reader / `println` is not stackless-lowerable, so it always
falls back to the native path where the relaxation applies. Verified: `f` with a
pure `(ok? r)` body lowers stackless, while the same shape with a
`(println (pm r))` body falls back to native. The stackless path therefore only
ever sees scalar-accessor bodies, which its strict check fully covers.

## Fix directions

1. **Replace the whitelist with a non-retention contract (the principled
   fix).** Give inline-C / stdlib sinks an explicit `#[borrow]`-style
   "reads-arg, does not retain a pointer into it" annotation, and drive
   `box_reader_result_void_sink` off that instead of a name list. `println`
   etc. carry the contract in stdlib; any annotated user sink joins the
   confinement set for free; the invariant is checked, not trusted. Mirrors the
   existing `^borrow` / `nonretain_param_mask` machinery.

2. **Result-flow escape analysis on the reader temp (composes with #1).**
   Rather than "is the parent a known sink?", ask "does the value the reader
   produced escape this scope?" via the existing escape walk. Admits arbitrary
   in-scope consumption, not just the sink shape. Residual hole is inline-C
   laundering -- exactly what #1's annotation closes -- so #1 + #2 are the
   complete story.

## Not worth it

- Refcounting the panic payload/message -- the archived report is explicit that
  these are header-less plain records; reintroducing drop-glue is out of scope.
- Refining the coarse scalar/nil scope-value gate in
  `catch_box_binding_reader_confined` -- rejects some safe struct-returning
  scopes, but the ROI is tiny.

## Recommendation

Not v1-blocking. Treat a request to **grow the sink list** as the trigger to do
#1 properly rather than appending another trusted name -- the list is documented
"known non-retaining" precisely so that trigger is visible.
