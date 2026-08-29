# A `nil` function-body tail is not checked against the declared return type

**Severity: medium** (silently wrong runtime value -- the function returns 0
where it declared `: int` / `: cstr` / anything -- but the shapes that reach it
are narrower than the general "wrong value" class, see below).

**Status:** OPEN. Filed 2026-08-29 while fixing
[nested-defn-accepted-outer-returns-zero](../archive/nested-defn-accepted-outer-returns-zero.md),
which is one instance of it. Found on Linux/gcc against `main` at `f3bbc148`.

## Summary

The function-body return-type check (TUR-E0707 / TUR-E0709) fires for every tail
type except `nil`. A `nil` tail in a function declared to return something is
accepted silently, and the function returns 0 at runtime.

## Repro

```turmeric
(defn f [] : int nil)
(defn main [] : int (f))
```

`tur check` is silent; the program exits 0.

## What narrows it -- only `nil` slips through

The check itself works. Varying just the tail expression in
`(defn f [] : int <tail>)`:

| Tail | Result |
| --- | --- |
| `"hello"` | `error [TUR-E0709]: function 'f' declares return type 'int' but its body ...` |
| `1.5` | `error [TUR-E0707]` |
| `true` | `error [TUR-E0709]` |
| `nil` | **accepted, returns 0** |

So this is not a missing check, it is one type escaping an existing one. It is
not specific to `: int` either -- `(defn g [] : cstr ... nil)` is accepted the
same way, which is worse, since the caller gets a null `char *` rather than a
plausible integer.

Multi-form bodies behave identically; the single-form case above is the minimal
repro.

## Why this matters more than the arity of the repro suggests

Nobody writes `(defn f [] : int nil)` on purpose. The reason to fix it is that
`nil` is what a *lot* of things collapse to, so this hole is the one that makes
other mistakes silent rather than loud:

- **Definitions.** `defmacro`, `defclass` and `deftype` all elaborate to
  `EX_NIL_LIT` once registered. A function body ending in one of those returns 0
  today. That is exactly the defect archived as
  `nested-defn-accepted-outer-returns-zero`, whose real-world cause was a missing
  close paren. TUR-E0713 now rejects the definition-in-tail-position case
  *specifically*, but only for the definition kinds that keep a distinct `Expr`
  kind plus an exact source-head match -- a definition that has collapsed to
  `EX_NIL_LIT` and arrived via macro expansion still slips through here.
- Anything else that elaborates to nil in a position the author believed was a
  value.

Fixing this would subsume TUR-E0713's job, which is the argument for doing it:
that diagnostic is a targeted patch over this hole, chosen because it could name
the actual cause ("check for a missing close paren"). A general fix here should
keep that message for the definition case rather than replacing it with a bare
type mismatch -- the specific diagnostic is what made the paren slip findable.

## Expected

`(defn f [] : int nil)` should be a TUR-E0709, the same as
`(defn f [] : int "hello")`.

## Fix direction

Find where the body-tail type is compared against the declared return (the
TUR-E0707 / TUR-E0709 site) and work out why `TY_NIL` is exempt. The exemption
is probably deliberate and probably about unannotated functions: `return_kind`
starts at `TY_NIL` in `elab_defn`, so "declared nil" and "not annotated" are the
same value there, and skipping the check is the cheap way to avoid rejecting
every unannotated function. If that is the reason, the fix is to distinguish
them -- carry a `return_annotated` flag (`elab_fn` already has one) and check
the tail whenever the return type was written down.

Watch for `: nil` / `: void` functions, which must keep accepting a nil tail,
and for the `return_kind == TY_NIL || return_kind == TY_TYVAR` fallback in
`elab_fns.c` that infers a return type from `body->type` -- that path relies on
the current looseness.

## Relationship to TUR-E0713

TUR-E0713 (`definition-in-tail-position`) is a special case of this, fixed
first because it could carry the paren-slip message. If this report is fixed
generally, revisit whether TUR-E0713 should remain as a more specific
diagnostic layered on top -- it should, unless the general error can name the
missing paren too.
