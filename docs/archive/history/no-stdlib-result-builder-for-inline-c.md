# No way to build a `result<T, E>` from inside an inline-C block

**Status:** Resolved (2026-06-14) -- the blessed path is **Option A**, already
implemented as the `tur_ok` / `tur_err` / `tur_some` / `TUR_NONE` preamble
helpers (the "C#1 (test-suite-idioms)" block emitted by
`src/compiler/emit_module.c:2298-2341`). These share the canonical
Option/Result heap layout with `stdlib/{option,result}.tur` byte-for-byte, so a
value built in inline-C flows transparently into the stdlib accessors. The
remaining gap this report named -- "there is no way to *type* the
returned-from-inline-C value as a real result" -- does **not** hold: an
inline-C `defn` can declare `: (Result A B)` / `: (Option A)` directly and
`return tur_ok(...)` / `tur_some(...)`; the carrier-ABI return path accepts it
(verified by the fixture below). What was actually missing was the *blessing*:
the helpers were undocumented outside an archived plan, so spice authors
hand-rolled the struct instead.

Shipped in this change:
- `docs/guides/c-integration-guide.md` -- new "Returning `result` / `option`
  from inline-C" section under Capability structs, with the helper table and a
  worked typed-handle example, plus the "don't re-declare the struct / don't
  abort on recoverable failure" guidance.
- `tests/fixtures/inline-c-typed-result-option/` -- end-to-end fixture: a
  fallible C constructor returns a *typed* `(Result Device int)` and
  `(Option Device)` over a `defopaque` handle, inspected by stdlib
  `ok?`/`err?`/`ok-val`/`some?`/`unwrap`.

No new named `_int` / `_ptr` variants were added (rejected redundant ABI
surface): the `int64_t` carrier already carries both integer codes and opaque
handles via the standard `(int64_t)(intptr_t)p` cast.

Downstream follow-up (separate repo, `../turmeric-spices`): `rtmidi` can now
delete `__ok` / `__err` and retype `midi-in-new` / `midi-in-open` et al. from
`: ptr<void>` to real `result<MidiIn, int>` -- tracked there, not blocked on
this repo.

---

**Original report (Reported)**

**Status:** Reported
**Severity:** Ergonomic / stdlib gap. Every spice that must allocate or
acquire a fallible resource in inline-C and return `result<Handle, E>` is
forced to hand-build the result struct manually, duplicating the
`{ bool is_ok; int64_t ok_val; int64_t err_val }` layout in raw C and
relying on it staying in sync with `stdlib/result.tur`. The audit's S3
class folds this in: it is the *constructor* side of "results collapsed
to `:int`".
**Discovered:** 2026-06-14, while scoping language pre-work for the
spice-wide `:int` audit fix-up.
**Surface:** stdlib (`stdlib/result.tur`) and the compiler's inline-C
boundary -- specifically, what (if any) helpers are emitted that inline-C
bodies may call to build option / result values.

---

## Summary

`stdlib/result.tur` defines `ok` and `err` as ordinary Turmeric `defn`s
that build the result struct via `make-struct`. They are callable from
Turmeric, not from C. The compiler also emits `tur_ok` / `tur_err`-style
helpers in the codegen path, but those are not part of a stable C ABI a
spice's inline-C body can rely on.

The result: any spice whose constructor *must* run in inline-C (because
the C library allocates the handle and there is no way to do the alloc
post hoc from Turmeric) has two options, both bad:

1. Re-declare the result struct layout inside the inline-C block,
   `malloc` it, fill the three fields by hand, and return it as
   `:ptr<void>`. This is what `rtmidi` does -- see citations below.
2. Skip `result` entirely and abort on failure. This is what stdlib does
   for its own allocators (threadpool, taskgroup, chan, future). It works
   if the failure mode is truly fatal but is wrong for fallible
   resources like opening a MIDI device or a network socket.

Neither is great; both are inherited by every downstream spice that
follows them.

## Observed

### `stdlib/result.tur`: `ok` / `err` are Turmeric-only

`ok` / `err` build the result via `make-struct`. They are not exposed as
C entry points, and there is no `extern-c` shim that an inline-C body
could call.

### `rtmidi/core.tur:17-31` -- canonical wart

```turmeric
;;; __ok -- create an ok result wrapping integer value v.
(defn __ok [v : int] : ptr<void>
  ```c
  #include <stdlib.h>
  struct { bool is_ok; int64_t ok_val; int64_t err_val; } *r = malloc(sizeof(*r));
  r->is_ok = true; r->ok_val = v; r->err_val = 0; return r;
  ```)

;;; __err -- create an err result wrapping integer error value e.
(defn __err [e : int] : ptr<void>
  ```c
  #include <stdlib.h>
  struct { bool is_ok; int64_t ok_val; int64_t err_val; } *r = malloc(sizeof(*r));
  r->is_ok = false; r->ok_val = 0; r->err_val = e; return r;
  ```)
```

These helpers (and worse: the open-coded equivalents embedded in
`midi-in-new` at `core.tur:71-85`, `midi-in-open` at `in.tur:51-61`, etc.)
re-declare the result struct layout in raw C. The function signature is
declared `: ptr<void>`, not `: result<int, int>`, because there is no way
to type the returned-from-inline-C value as a real `result` -- the type
checker treats `make-struct`'d results as opaque pointers from inline-C's
point of view.

The cost spreads: every callsite that consumes one of these helpers must
either bury more inline-C to peek at `is_ok`/`ok_val`/`err_val`, or
remember to cast back to a `result<int, int>` and call `ok?`/`ok-val` --
which works only because the layout happens to match by hand. Any drift
in the canonical result layout silently misreads every spice that copied
the pattern.

### stdlib precedent: don't wrap fallible allocators in result

`stdlib/threadpool.tur`, `stdlib/taskgroup.tur`, `stdlib/chan.tur`,
`stdlib/future.tur`, `stdlib/reactor.tur` -- all expose constructors that
return the opaque directly and `fprintf(stderr, ...); abort()` on OOM in
the inline-C body. Examples:

- `stdlib/taskgroup.tur:76-95` -- `task-group-new : TaskGroup` aborts on
  OOM.
- `stdlib/threadpool.tur` -- `threadpool-new : ThreadPoolHandle` ditto.

This is a real design choice for *unrecoverable* failures (process is in
trouble; can't allocate a control block). It is not a substitute for
`result` on operations like "open a MIDI device" or "connect to a
database", which are routinely failure-prone for non-fatal reasons.

The unfortunate side-effect: stdlib offers no worked example of "inline-C
constructor that returns `result<Handle, E>`", so spice authors have
copied the rtmidi wart instead.

## Expected

A blessed way to build a `result<T, E>` (and, by extension, `option<T>`)
from an inline-C block. Candidates:

### Option A: stdlib-emitted C helpers (`tur_ok_int`, `tur_err_int`, etc.)

Emit a small set of stable C functions exposed in a header that inline-C
bodies may include. Shapes:

```c
/* tur_result.h -- emitted by the compiler from stdlib/result.tur */
void *tur_ok_int(int64_t v);
void *tur_err_int(int64_t e);
void *tur_ok_ptr(void *v);
void *tur_err_ptr(void *e);
void *tur_some_int(int64_t v);
void *tur_none(void);
```

Implemented inside the compiler runtime alongside the codegen for
`stdlib/result.tur` (so the layout stays in lockstep). Spices include the
header from their inline-C and never re-declare the result struct.

Pros: minimal language change; immediate ergonomic win; layout stays
canonical. Cons: spreads ABI surface; needs an `#include` story for
spices.

### Option B: extern-c shim for `ok`/`err`

Mark the Turmeric `ok` / `err` defns as exported via a stable C ABI
(`extern-c "tur_result_ok_int"` etc.). Spices link against the existing
result symbols rather than a parallel header. Same effect as A, lower
total surface, but requires the closure-rep work to be friendly to
extern-c for parametric constructors (`ok<T>` is templated on `T`; only
the monomorphised entries exist as symbols).

### Option C: a builder macro

`(result-from-c [...inline-c...] : result<T, E>)` form that expands to
the inline-C body wrapping `tur_ok` / `tur_err` in scope. Compiler-level
sugar around Option A. Best ergonomics but the heaviest implementation.

### Option D (combined with the language gap above)

`docs/reported/typed-c-abi-function-pointers.md` proposes adding
`extern-fn` / `c-fn` to the type system. If that work lands, the same
mechanism can expose `ok` / `err` cleanly to inline-C without the parallel
header. Worth co-designing.

## Validation

For any of the above:

- `rtmidi/core.tur:17-31` can delete `__ok` / `__err`. Constructors call
  the blessed helper from inline-C and declare a real `result<T, E>`
  return type.
- A new stdlib fixture verifies the layout: ABI-test that
  `tur_ok_int(7)` (or equivalent) is `ok?`-true and `ok-val` returns `7`
  in Turmeric code.
- The `:int` audit's S3 sites that currently return `: ptr<void>` for
  fake results (rtmidi `midi-in-new`, `midi-in-open`, etc.) can be
  retyped to `: result<MidiIn, int>` without redeclaring layout.

## Cross-references

- `docs/reported/spices-int-stand-in-audit-2026-06-14.md` -- the audit
  this finding supports.
- `docs/reported/typed-c-abi-function-pointers.md` -- companion language
  gap; co-design candidate (Option D above).
- `stdlib/result.tur` -- the type whose constructors are out of reach
  from inline-C.
- `stdlib/threadpool.tur`, `stdlib/taskgroup.tur`, `stdlib/chan.tur`,
  `stdlib/future.tur`, `stdlib/reactor.tur` -- precedent for "abort, do
  not wrap"; deliberately not the pattern we want spices to follow for
  fallible-but-non-fatal resources.
- `CLAUDE.md` -- "No Lazy `:int` Stand-Ins -- STRICT RULE" (added
  2026-06-14) -- declaring a result-shaped return as `: ptr<void>`
  because the constructor lives in inline-C is exactly the kind of
  workaround that rule expects to be surfaced rather than propagated.
