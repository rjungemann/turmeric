---
status: RESOLVED 2026-07-25
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

## Resolution (2026-07-25)

Direction #1, but **inferred rather than annotated** -- no new syntax was
needed. The confinement question ("is this value discarded, or handed only to
something that will not retain it?") is already answered by `box_uses_confined`;
applying that same walk to a *callee's own body*, at the point the defn is
elaborated, answers "does this function retain that parameter?" for free.

- `Binding` gains `nonretain_ptr_param_mask`: bit i set when parameter i is a
  pointer-carrying scalar (`cstr` / `ptr<void>`) whose every use in the body is
  discarded or flows into another non-retaining sink. Inferred in
  `elab_fns.c` beside the existing `nonretain_param_mask`, guarded by the same
  `expr_subtree_has_inline_c` check -- load-bearing here too, since a C body can
  stash the pointer where no AST walk can see it.
- The result gate mirrors `catch_box_binding_reader_confined`: a parameter can
  only be treated as non-retained when the function's own result cannot carry it
  back out (non-pointer scalar or nil return).
- The `EX_CALL` arm of `box_uses_confined` now consults that mask **per
  argument**, not per call, so a two-parameter logger that retains one argument
  and prints the other is handled precisely.

The hardcoded list survives as the base case for compiler *builtins* (`println`
and friends live in `builtins.c`, not stdlib, so there is no body to analyse).
It is no longer the only path, which is what made it a footgun: a user-defined
sink is now judged on what it does, not on whether someone remembered to add its
name.

Measured before and after on the motivating shape -- `(my-log (panic-msg r))`
where `my-log` only prints -- via the emitted C: `tur_result_box_free` absent
before, present after, matching `println`. The two negative cases stay refused:
a sink that stores its argument into a struct field, and a sink with an inline-C
body. Pinned by `tests/fixtures/catch-box-user-sink-confines`.

Note the fixture asserts on program output rather than on the free. Whether the
box is freed is invisible at runtime -- freeing is correct, not freeing is a
bounded leak -- so the free/no-free decision is asserted at the codegen level in
`expected.c`, and the fixture pins that all three sink shapes still compile and
run correctly.

Also fixed here: `catch_box_binding_reader_confined` carried an **unguarded
leftover `fprintf(stderr, "[boxfree] ...")`**, which printed on every `tur
build` / `tur emit-c` of a program with a let-bound caught box read through a
reader. The suite never caught it because fixtures compare stdout. Found by
reading the function this report points at.

### Not done, and why

Direction #2 (result-flow escape analysis on the reader temp) was not needed for
the motivating case and is not obviously worth it now: with #1 inferred rather
than annotated, the set of admitted sinks widened to "anything whose body does
not retain", which covers the shapes that were leaking. Revisit if a real case
turns up that #1 still rejects.

## Not worth it

- Refcounting the panic payload/message -- the archived report is explicit that
  these are header-less plain records; reintroducing drop-glue is out of scope.
- Refining the coarse scalar/nil scope-value gate in
  `catch_box_binding_reader_confined` -- rejects some safe struct-returning
  scopes, but the ROI is tiny.

## Recommendation (historical)

Not v1-blocking. Treat a request to **grow the sink list** as the trigger to do
#1 properly rather than appending another trusted name -- the list is documented
"known non-retaining" precisely so that trigger is visible.

That trigger is now largely moot: the list only governs builtins, and user code
is inferred. Adding a *retaining* builtin to it would still be a soundness bug,
so the comment above it still matters.
