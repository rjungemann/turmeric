# A `nil` function-body tail is not checked against the declared return type

**Severity: medium** (silently wrong runtime value -- the function returns 0
where it declared `: int` / `: cstr` / anything -- but the shapes that reach it
are narrower than the general "wrong value" class, see below).

**Status:** RESOLVED 2026-09-02. Filed 2026-08-29 while fixing
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

## Resolution (2026-09-02)

`return_type_nil_body_conflict` joins the return-position dispatcher as
`RET_CONFLICT_NIL_BODY`, checked BEFORE the representation-pair predicates and
NOT gated on the return class -- every tolerance those classes buy is a bridge
between two things that both carry a value and are both `int64_t` in the emitted
C, and a nil body carries no value to bridge. `(defn f [] : int nil)` is a
TUR-E0709 now, as the report asked.

The report's root-cause guess was right on the mechanism and slightly off on the
shape. It predicted an *exemption* for `TY_NIL`; there was none. The dispatcher
is a list of predicates each targeted at one confusable PAIR (`float`-vs-int
register class, `cstr`-vs-int pointer/scalar, `bool`-vs-integer), and nil simply
had no predicate. So this was not one type escaping a check, it was a missing
member of a set -- which is why adding one predicate closes it with no
re-plumbing.

Its warning about `return_kind` was exactly right and load-bearing: the
elaborator initializes `return_kind` to `TY_NIL`, so "declared `: void`" and
"not annotated" are the same TypeKind. The defn path gained a
`return_annotated` flag mirroring `elab_fn`'s, set by observing whether the
annotation block advanced `body_start` -- one read of the cursor rather than a
flag at each of its five exits, which is the version that cannot miss one.

### The scope line, and the measurement behind it

The check fires on a nil **literal** tail, not on any nil-**typed** tail. That
line was drawn from a measurement, not a preference.

Checking every nil-typed tail also rejects `(defn main [] : int (println ...))`
-- a void call in tail position -- which is **25 fixtures in this corpus** and
the idiomatic entry point, where the 0 a nil tail already produces is the exit
code the author wants. Whether that shape should be an error is a real question
and a much larger one; the reported defect does not need it. Every shape the
report argues from lands on the literal: `defmacro` / `defclass` / `deftype`
collapsing to `EX_NIL_LIT` once registered, and a missing close paren swallowing
the real tail. Pinned as an exemption rather than left implicit, so the line is
visible if someone revisits it.

`body_tail_is_nil_literal` peels `EX_DO` / `EX_LET` / `EX_LETREC`. Testing
`body->kind == EX_NIL_LIT` directly let `(defn f [] : int (println "x") nil)`
through while rejecting the single-form version -- exactly the inconsistency the
report says does not exist ("multi-form bodies behave identically"). `EX_IF` is
deliberately not peeled: its branch types are already unified and checked against
each other, `!`-typed panic arms included.

### Relationship to TUR-E0713 -- both stay

The report asked that the specific paren message survive, and it does: a
definition written literally in tail position still gets TUR-E0713 (`function
'f' ends its body with a definition (defmacro), so it has no value to return`),
which fires first.

The general check also closes the residual the report identified but could not
fix: TUR-E0713 needs a distinct `Expr` kind plus an exact source-head match, so
a definition arriving via MACRO EXPANSION has already collapsed to `EX_NIL_LIT`
and slipped past it. That now raises TUR-E0709. Pinned by
`tests/fixtures/errors/nil-tail-macro-expanded-definition/`. One cosmetic note:
its span points at the macro's own definition rather than the use site, which is
span provenance through expansion, not a fault in the check.

### Instance methods: deliberately not included

`return_position_conflict`'s other caller is the typeclass instance-method path,
which passes `check_nil_body = false`. A class-declared method return IS always
written down, so turning it on is defensible and is a one-line change with a
diagnostic already waiting in its switch -- but it is a separate blast radius
from the defn case, and mixing them would make a regression in either
unattributable.

### Verification

Suite 2751 passed / 0 failed, zero snapshot churn. Pinned by
`tests/fixtures/errors/nil-tail-under-declared-return/` (the multi-form literal,
the shape a missing paren leaves),
`tests/fixtures/errors/nil-tail-macro-expanded-definition/` (the E0713
residual), and `tests/fixtures/nil-tail-exemptions/` -- the last covering all
three shapes that must keep working, each with the reason getting it wrong turns
this fix into a language break.
