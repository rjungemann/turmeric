---
title: Returning `Result` / `Option` from Inline-C
category: Interoperability
description: Build typed Result/Option values inside inline-C bodies with the preamble helpers (tur_ok_ptr, tur_err_int, tur_some_ptr, tur_none, ...) instead of hand-rolling the struct or returning a magic-sentinel :int
---

# Returning `Result` / `Option` from Inline-C

A fallible C constructor -- one that allocates or acquires a handle in C
and can fail (open a MIDI port, connect a socket, parse a file) -- should
return a real `(Result Handle E)` or `(Option Handle)`, **not** a
`:ptr<void>` and **not** a magic-sentinel `:int` (`-1`, `0`-as-absent,
`INT64_MIN`). A sentinel throws away the type the checker could otherwise
enforce, and forces every caller to remember the convention.

You do not have to hand-roll the result struct. Every emitted translation
unit carries a small set of preamble helpers that build and inspect
Option/Result values through the **canonical** heap layout -- the same one
[`stdlib/option.tur`](https://github.com/rjungemann/turmeric/blob/main/stdlib/option.tur)
and [`stdlib/result.tur`](https://github.com/rjungemann/turmeric/blob/main/stdlib/result.tur)
use -- so a value built in C flows straight into the stdlib accessors
(`ok?`, `err?`, `ok-val`, `err-val`, `some?`, `unwrap`) and vice versa.

The builders return the `int64_t` **carrier** -- a heap pointer to the
canonical layout. That is still what an inline-C body should hand back, and
nothing here changed when concrete monomorphs started flowing by value: the
compiler bridges the carrier into the aggregate at the boundary, and a
`(none)` carrier (the null pointer) survives the crossing. What the boundary
needs is a real declared type on the C function -- `: (Result MidiIn int)`,
not `: int`. A producer declared `: int` hands back a bare word the accessors
cannot type, so its callers have to write `(ok? (:: r (Result int int)))` at
every use site.

## The helpers

The builders come in three flavours. Prefer the **typed** `_int` / `_ptr`
builders: they spell out the payload's cast direction at the call site, so
an inline-C author never has to remember whether a pointer payload needs an
`(int64_t)(intptr_t)` widening.

| Helper | Builds | Payload |
|--------|--------|---------|
| `tur_ok_ptr(void *p)`   | `(Result A B)`, ok  | pointer handle, widened for you |
| `tur_ok_int(int64_t v)` | `(Result A B)`, ok  | integer code, passed as-is |
| `tur_err_ptr(void *p)`  | `(Result A B)`, err | pointer handle, widened for you |
| `tur_err_int(int64_t e)`| `(Result A B)`, err | integer code, passed as-is |
| `tur_some_ptr(void *p)` | `(Option A)`, some  | pointer handle, widened for you |
| `tur_some_int(int64_t x)`| `(Option A)`, some | integer payload, passed as-is |
| `tur_none()`            | `(Option A)`, none  | -- (NULL) |

`tur_none()` is the function-call companion to the `TUR_NONE` macro; use
whichever reads better next to the typed builders around it.

Inspectors, for an inline-C body that *consumes* a Result/Option built
elsewhere (the carrier is the `int64_t` the opaque/Result lowered to):

| Helper | Reads |
|--------|-------|
| `tur_is_ok(int64_t r)`    | `bool` -- is the result ok? |
| `tur_ok_value(int64_t r)` | the ok payload (as `int64_t`) |
| `tur_err_value(int64_t r)`| the err payload |
| `tur_is_some(int64_t o)`  | `bool` -- is the option some? |
| `tur_opt_value(int64_t o)`| the some payload |

### The untyped `tur_box_*` builders

The typed builders are thin wrappers over the original carrier-level
builders, which take the raw `int64_t` carrier directly:

| Helper | Builds |
|--------|--------|
| `tur_box_ok(int64_t v)`   | `(Result A B)`, ok, payload `v` |
| `tur_box_err(int64_t e)`  | `(Result A B)`, err, payload `e` |
| `tur_box_some(int64_t x)` | `(Option A)`, some, payload `x` |
| `TUR_NONE`                | the none `(Option A)` (NULL) |

These remain valid and are how older inline-C blocks in the tree call into
the layout. For a pointer payload they require the explicit
`tur_box_ok((int64_t)(intptr_t)h)` cast -- which is exactly the friction
`tur_ok_ptr(h)` removes. Reach for `tur_box_*` only when the payload is
already an `int64_t` carrier you are forwarding unchanged; otherwise prefer
the `_int` / `_ptr` builder that names your intent.

## Who frees the box

**Ownership of the box is being settled by in-flight work; do not add manual
frees on the strength of this section.**

The builders `malloc` a 16-byte carrier box. On `main` as of 2026-09-02 nothing
frees it on the erased path -- a generic signature whose payload is a type
variable -- so `stdlib/result.tur`'s `result-bimap` and `stdlib/weak.tur`'s
`weak/upgrade` each leak one box per call (measured).

`origin/claude/sum-representation-option-niche-nh58fn` fixes this, under a
contract worth knowing before you write such a body: **an inline-C body whose
DECLARED return is `(Option T)` / `(Result T E)` transfers ownership of the box
to its caller**, and the compiler frees it at read-back. Two consequences it
records -- return a FRESH box (a cached static returned twice becomes a double
free), and a BORROWED box needs a different signature (declare the element type
and let the caller wrap it, as `vec-get` does).

So the useful advice today is about what you return, not about freeing:

- Return a fresh box from each call.
- If the box belongs to something else, do not declare the result as an
  Option/Result.

**Do not sprinkle `option-free` / `result-free` to chase the current leak.**
They become double frees when that branch lands -- the same trap `arc.tur` hit
from the other side, where its explicit `option-free` calls had to be REMOVED
once its Option became by-value. And they are already wrong on a concrete
monomorph, which flows BY VALUE with no allocation at all:

```
ERROR: AddressSanitizer: attempting free on address which was not malloc()-ed
```

Background: [docs/upcoming/reclamation-plan.md](https://github.com/rjungemann/turmeric/blob/main/docs/upcoming/reclamation-plan.md)
on that branch, and
[docs/reported/inline-c-option-carrier-box-leaks.md](https://github.com/rjungemann/turmeric/blob/main/docs/reported/inline-c-option-carrier-box-leaks.md).

## Worked example: an rtmidi-shaped port constructor

A C MIDI binding wraps `RtMidiInPtr` -- an opaque library handle that
`rtmidi_in_create_default()` returns and that the close call consumes. The
constructor can fail (the backend is unavailable, the requested port index
is out of range), so it returns a typed `(Result MidiIn int)`: ok carries
the handle, err carries an integer status code.

```turmeric
;; A linear opaque over the C library handle -- consumed exactly once by
;; midi-in-close. See opaques-guide.md for the :linear discipline.
(defopaque MidiIn :ptr<void> :linear)

;; midi-in-open : port -> (Result MidiIn int)
;;   ok  = the live RtMidiInPtr, built with tur_ok_ptr (no hand cast)
;;   err = an integer status code, built with tur_err_int
(defn midi-in-open [port : int] : (Result MidiIn int)
  ```c
  RtMidiInPtr h = rtmidi_in_create_default();
  if (h == NULL)        return tur_err_int(1);   /* backend unavailable */
  if (!h->ok)           { rtmidi_in_free(h); return tur_err_int(2); }
  unsigned n = rtmidi_get_port_count(h);
  if ((unsigned)port >= n) { rtmidi_in_free(h); return tur_err_int(3); }
  rtmidi_open_port(h, (unsigned)port, "turmeric-in");
  return tur_ok_ptr(h);
  ```)

;; midi-in-close : consume the handle (linear: exactly one call).
(defn midi-in-close [m : MidiIn] : void
  ```c
  RtMidiInPtr h = (RtMidiInPtr)(intptr_t)m;
  rtmidi_close_port(h);
  rtmidi_in_free(h);
  ```)
```

The consumer is plain Turmeric -- the stdlib accessors read the C-built
value with no special handling:

```turmeric
(defn with-first-port [] : int
  (let [r (midi-in-open 0)]
    (if (ok? r)
      (let [m (ok-val r)]
        (do
          (poll-loop m)
          (midi-in-close m)
          0))
      (err-val r))))          ;; surface the status code on failure
```

An `(Option MidiIn)` "open the default port if there is one" variant uses
`tur_some_ptr` / `tur_none` the same way:

```turmeric
(defn midi-in-default [] : (Option MidiIn)
  ```c
  if (rtmidi_get_port_count_default() == 0) return tur_none();
  RtMidiInPtr h = rtmidi_in_create_default();
  rtmidi_open_port(h, 0, "turmeric-in");
  return tur_some_ptr(h);
  ```)
```

## Why this beats the alternatives

This is the blessed replacement for two anti-patterns that used to spread
through spices (see
[docs/archive/history/no-stdlib-result-builder-for-inline-c.md](https://github.com/rjungemann/turmeric/blob/main/docs/archive/history/no-stdlib-result-builder-for-inline-c.md)):

- **Re-declaring the struct in raw C** -- `struct { bool is_ok; int64_t
  ok_val; int64_t err_val; } *r = malloc(...)` returned as `:ptr<void>`.
  This duplicates the layout, drifts silently if the canonical layout ever
  changes, and discards the `Result` type. **The drift is no longer
  hypothetical: SR2b changed the canonical layout to the tagged sum
  `{ int tag; union { ... } as; }`, so any surviving hand-rolled copy of the
  struct above is now reading the wrong bytes.** The preamble carries a
  `_Static_assert` pinning `tur_option_t` / `tur_result_box_t` to their
  byte layout precisely so the *helper* path cannot drift; a hand-rolled
  copy gets no such guard.
- **Aborting on failure** -- `fprintf(stderr, ...); abort()`. That is the
  right call for a genuinely unrecoverable allocation (a control block the
  process cannot run without -- this is why `threadpool-new` /
  `task-group-new` abort), but it is wrong for routine, recoverable
  failures like a port that is busy or a connection that is refused.

## Limitation: only `_int` and `_ptr` payloads

v1 ships the monomorphised `_int` / `_ptr` builders, which cover an integer
error code or an opaque pointer handle -- the shape every audited spice
site needs. A `(Result MyStruct MyErr)` where `MyStruct` / `MyErr` are
user-defined *by-value* types is **not** constructible from inline-C with
these helpers: the payload has to fit the single `int64_t` carrier slot.
Wrap the value behind an opaque pointer handle (the rtmidi pattern above)
or construct the `Result` in Turmeric instead.

## A control form around an `if` over these builders

The builders below return the int64 CARRIER, and the consumer bridges it to the
by-value aggregate. When an `if`'s arms are both carrier producers, `emit_if`
bridges each arm into its merge temp -- and a `let` or `do` wrapping that `if`
used to bridge the already-concrete result a second time:

```c
__t172 = (*(tur_adt_Result__Handle__int *)(intptr_t)(__t174));  /* already a struct */
```

`tur check` was silent; it failed at `cc` with `operand of type 'tur_adt_...'
where arithmetic or pointer type is required`, naming no `.tur` line. The same
`if` as the whole function body always worked, which is what made the workaround
("hoist the block into its own defn") effective and the cause obscure.

Fixed 2026-09-02; both the `let` and `do` wrappers are pinned by
`tests/fixtures/control-form-around-if-carrier-arms/`. Nothing about how you
write the inline C changes -- this is recorded because the shape it broke is the
one this guide recommends, so an older compiler will still reject it. See
[docs/archive/control-form-around-if-double-unboxes-carrier-arms.md](https://github.com/rjungemann/turmeric/blob/main/docs/archive/control-form-around-if-double-unboxes-carrier-arms.md).

## See also

- [opaques-guide.md](opaques-guide.md) -- the `defopaque` handles these
  constructors hand back, and the `:linear` / `:affine` discipline.
- [c-integration-guide.md](c-integration-guide.md) -- inline-C blocks, the
  `int64_t` carrier, and the FFI boundary in full.
- [error-handling-guide.md](error-handling-guide.md) -- `Result` / `Option`
  on the Turmeric side: the accessors these C-built values flow into.
- `tests/fixtures/inline-c-result-builder/` -- end-to-end fixture
  exercising every typed builder against the stdlib accessors.
