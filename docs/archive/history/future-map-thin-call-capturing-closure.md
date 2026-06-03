---
title: future-map / future-then thin-call their callback (capturing closure segfaults)
category: Reported Bug
severity: medium (latent segfault on a capturing-closure argument; captureless works)
status: resolved
---

# future.tur `future-map` / `future-then` thin-call their callback

## One-line summary

`stdlib/future.tur`'s `future-map` and `future-then` invoke their callback
parameter through a **thin** inline-C cast `((int64_t (*)(int64_t))fn)(val)`.
A captureless function works (it *is* a bare function pointer), but a
**capturing** closure is a fat box `{ thunk, env... }` -- the thin cast reads the
box's address as code and jumps to garbage (segfault). Severity: **medium** --
latent; only triggers when a closure that closes over state is mapped over a
future.

## Observed vs. expected

- Captureless: `(future-map fut (fn [x] (+ x 1)))` -- works (bare fn pointer).
- Capturing: `(let [k 10] (future-map fut (fn [x] (+ x k))))` -- the inline-C
  does `((int64_t (*)(int64_t))fn)(val)` where `fn` is the box pointer, so it
  calls the box as if it were `int64_t(*)(int64_t)` -> **segfault**.

Expected: both work, the callback fat-dispatched through slot 0 of the box.

## Root cause (`stdlib/future.tur`)

`future-map [f :ptr<void> fn :ptr<void>]` body:

```c
out->value = ((int64_t (*)(int64_t))fn)(val);   /* thin bare-pointer call */
```

`fn` is typed `:ptr<void>` and invoked with the *thin* calling convention.
After closure-representation-unification Phase 3 / Option B, a capturing closure
is a fat box, and the matching invocation is the fat protocol (read slot 0 as
the thunk, call `thunk(box, val)`). `future-then` has the same pattern.

This is the same class of bug B-3 fixed for the `*-eq?` carrier helpers, but it
lives in an **inline-C** body, so it is *not* caught by B-4's Turmeric
direct-call gate (the gate only fires on a Turmeric `(fn ...)` call head, not on
an inline-C cast) -- which is why it survives as a latent defect.

## Why it was deferred (not fixed in the Phase-3 wave)

B-3 migrated the `*-eq?` comparator helpers; B-4 closed the Turmeric-level
`:ptr<void>`-direct-call overload. `future-map`/`future-then` are a separate
inline-C surface that no current fixture exercises with a *capturing* callback,
so the suite stayed green and the defect did not surface during Phase 3.

## Proposed fix

Mirror the B-3 treatment: make the callback parameter `^fat fn` and fat-dispatch
it in the inline-C body:

```c
out->value = ((int64_t (*)(void *, int64_t))(intptr_t)
                  ((int64_t *)(intptr_t)fn)[0])((void *)(intptr_t)fn, val);
```

A captureless argument auto-boxes at the `^fat` call site; a capturing closure
passes through as a box. Apply to both `future-map` and `future-then` (and audit
the rest of `future.tur` -- e.g. any other `:ptr<void>` callback parameter
invoked via inline-C).

## How to validate a fix

- New fixture: map/then a future with a *capturing* comparator/transform that
  closes over a local, asserting the captured value is used (would segfault
  pre-fix). Cover int and float results (register-class-distinct).
- `bash tests/run.sh`: 0 FAIL.

## Resolution (2026-06-03)

Fixed in `stdlib/future.tur`: `future-map` and `future-then` now take a
`^fat fn` parameter and fat-dispatch the callback through slot 0 of the box
inside their inline-C bodies (`((int64_t (*)(void *, int64_t))box[0])(box,
val)`), exactly the proposed fix. A captureless argument auto-boxes at the
`^fat` call site; a capturing closure passes through as a box. Regression
fixture `tests/fixtures/future-capturing-closure/` maps and flat-maps futures
with closures that close over a local (would have segfaulted pre-fix) and
covers the error-propagation path. Full suite green (1312 passed, 0 failed).

While wiring the fixture (the first consumer to `(load "stdlib/future.tur")`
and actually emit `future-get`) a separate latent defect surfaced: `future-get`
had a Turmeric `;;` comment *inside* its inline-C block, which emitted invalid
C (`;; Build a Result to return`). It had never been compiled because every
prior fixture defined its own local `future-get`. Fixed by switching it to a C
`/* ... */` comment.
