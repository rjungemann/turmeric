# Plan: Type System C-ABI Surface (typed fn-ptrs + inline-C result builder)

> **Status:** Archived as the typed-`(c-fn ...)` half (TC0-TC4) -- which
> is what this plan actually delivered. TC5-TC9 (inline-C result/option
> helpers, docs, audit edits, rtmidi pilot, umbrella tracking) were
> split out into `docs/upcoming/type-system-c-abi-followups-plan.md`
> and are tracked there. The typed `(c-fn [A...] B)` surface is live
> and consumed by `tourist-session/store.tur` and `rtmidi/in.tur`'s
> callback site.
> **Last Updated:** 2026-06-23
> **Type:** Compiler + stdlib
> **Triggers:**
>   - `docs/reported/spices-int-stand-in-audit-2026-06-14.md` (the audit)
>   - `docs/reported/typed-c-abi-function-pointers.md` (S1 language gap)
>   - `docs/archive/history/no-stdlib-result-builder-for-inline-c.md` (S3 stdlib gap)
> **CLAUDE.md gate:** "No Lazy `:int` Stand-Ins -- STRICT RULE" (2026-06-14)
>   blocks any new `:int` stand-in writeup; this plan is the prerequisite
>   that makes the audit fix-up actually achievable without new
>   workarounds.

---

## Overview

The spice-wide `:int` audit (filed 2026-06-14) identified ~50 sites where
`:int` is used as a stand-in for a real type across 19 of 35 spices. Two
sub-classes of finding -- **S1** (callback parameters typed `:int`) and
**S3** (`result<T,E>` / `option<T>` return types collapsed to `:int` or
`:ptr<void>` because the constructor lives in inline-C) -- are not
**mechanically fixable** with the language and stdlib as they stand today.
Patching the audit's S2 (opaque handles) is mechanical; S1 and S3 require
language and stdlib pre-work.

This plan delivers that pre-work as a single coordinated change:

1. A **typed C-ABI function-pointer type** (`(c-fn [A...] B)`) distinct
   from the closure-bearing `(fn [A...] B)`, lowering to a bare
   `B (*)(A...)` at the ABI.
2. A **stdlib result/option builder callable from inline-C** so
   constructors that must allocate in C can return real `result<T,E>` /
   `option<T>` instead of `:ptr<void>` with a hand-rolled struct.

Once both ship, the audit's remaining S1/S3 work is mechanical signature
changes per spice. S2 work can land in parallel; it does not block on
this plan.

---

## Background

### Why S1 cannot be fixed today

`docs/reported/typed-c-abi-function-pointers.md`:

- `src/compiler/types.h:76-174` defines `TY_FN` (closures) and
  `TY_PTR_VOID` (raw pointers) but no `TY_CFNPTR`.
- A `(fn [A...] B)` value lowers two different ways depending on
  whether it captures: bare code pointer (captureless) or fat
  `{ thunk; env }` box (capturing). The type system does not
  distinguish them. Calling a fat box through a bare-pointer-shaped
  C signature crashes (see
  `docs/archive/captureless-closure-not-boxed-at-fat-ptr-void-boundary.md`).
- The closure-representation-unification work (B-0..B-4, 2026-06-03)
  unified closure layout but did not add a typed C-ABI surface; the gap
  persists.

Today's only options for an S1 site are `callback : int` or
`defopaque Cb :ptr<void>`. Neither catches a wrong-shape callback at
compile time.

### Why S3 (inline-C constructors) cannot be fixed today

`docs/archive/history/no-stdlib-result-builder-for-inline-c.md`:

- `stdlib/result.tur`'s `ok` / `err` use `make-struct` -- Turmeric-only,
  not callable from inline-C.
- Any spice whose constructor must run inside an inline-C block (because
  the C library allocates the handle and there is no way to do that
  alloc in Turmeric and then C-init it) is forced to hand-build the
  `{ bool is_ok; int64_t ok_val; int64_t err_val }` struct and declare
  the return as `:ptr<void>` instead of `result<T,E>`.
- `rtmidi/core.tur:17-31` and the open-coded variants at `core.tur:71-85`,
  `in.tur:51-61` are the canonical wart.
- Stdlib's own allocators (threadpool, taskgroup, chan, future, reactor)
  sidestep this by aborting on OOM -- correct for fatal failures, wrong
  for fallible-but-non-fatal resources (open MIDI device, open DB
  connection, etc.).

---

## Scope

### In scope for v1

- **Type-system change**: new `TY_CFNPTR` kind and `(c-fn [A...] B)` /
  `(c-fn [A...] -> B)` type-expression syntax.
- **Elaborator change**: conversion site that accepts a captureless
  `(fn ...)` (or an `extern-c`-declared function) and rejects capturing
  closures with a diagnostic pointing at the captures.
- **Codegen change**: lower `(c-fn [A...] B)` as `B (*)(A...)`.
- **stdlib runtime helpers**: stable C functions
  (`tur_ok_int`, `tur_err_int`, `tur_ok_ptr`, `tur_err_ptr`,
  `tur_some_int`, `tur_some_ptr`, `tur_none`) implemented alongside the
  codegen for `stdlib/result.tur` / `stdlib/option.tur` so layout stays
  in lockstep.
- **stdlib public C header**: `runtime/tur_result.h` (or equivalent),
  emitted into the build sysroot, `#include`-able from any spice's
  inline-C body.
- **stdlib documentation**: a guide section showing the canonical
  "inline-C constructor returns `result<Handle, E>`" pattern with a
  worked example mirroring rtmidi's `midi-in-new`.
- **Tests**: at least four new fixtures (see Validation).

### Out of scope for v1

| Future enhancement | Description |
|---|---|
| Generic `c-fn` parametricity | `(c-fn [A...] B)` as a generic type parameterised by C-callable signatures. v1 only supports concrete signatures at use sites. |
| `extern-fn` declaration form | Declaring an existing C function as a value of `(c-fn ...)`. v1 only supports conversion from captureless Turmeric defns. (Likely follow-up; not load-bearing for the audit.) |
| `tur_ok` / `tur_err` for arbitrary `T` | v1 ships the four monomorphised entries (`_int`/`_ptr`). Generic `T` requires more codegen plumbing and is not needed for the audit's S3 sites. |
| Migration of the 5 S1 sites + 19 S2/S3 spices | Tracked by the audit document; this plan unblocks them but does not implement them. |
| Compiler-side macro sugar (`result-from-c`) | The "Option C" sugar from the inline-C-builder finding. Useful but not load-bearing. |

---

## Design

### Surface syntax: `(c-fn [A...] B)`

Type-expression form. Distinct keyword from `fn`. Accepted anywhere a
type expression is accepted:

```turmeric
;; A typed C-ABI callback parameter.
(defn midi-in-set-callback
  [mi : MidiIn callback : (c-fn [float ptr<u8> int ptr<void>] void)]
  : void
  ```c
  rtmidi_in_set_callback((RtMidiInPtr)(intptr_t)mi,
                         (RtMidiCCallback)(intptr_t)callback,
                         NULL);
  ```)
```

The arrow variant `(c-fn [A...] -> B)` is also accepted for visual
parity with arrow notation in other type forms; both parse to the same
`TY_CFNPTR`.

### Conversion: captureless defn -> `(c-fn ...)`

A defn whose body has no captured environment (verified by the
elaborator's capture-set computation) and whose signature matches the
target `(c-fn ...)` lowers to a bare C function pointer with no implicit
env. Passing such a defn where a `(c-fn ...)` is expected is an implicit
upcast.

A capturing defn at the same site is a hard error
(`TUR-E0NNN: cannot convert capturing closure to (c-fn ...)`; cite the
captures; suggest `extern-c` + a static helper).

A wrong-arity / wrong-element-type defn at the same site is a hard
error (`TUR-E0NNN: signature mismatch ... expected (c-fn [A...] B) got ...`).

### TypeKind

```c
typedef struct {
  Type  *ret;
  Type **args;
  int    nargs;
} CFnPtrInfo;

/* in types.h */
typedef enum {
  /* ... existing kinds ... */
  TY_CFNPTR,  /* NEW */
} TypeKind;
```

`TY_CFNPTR` carries the signature directly; no `as.fn.boxed` flag (there
is no fat representation). Equality is structural over `ret` and `args`.

### Codegen

```c
/* (c-fn [int cstr] int) lowers to: */
typedef int64_t (*__tur_cfnptr_NNN)(int64_t, char *);
```

Function pointer typedefs are name-mangled per-signature into the
compilation unit's header so each unique signature exists once.

### stdlib runtime helpers

Emit a header alongside the runtime that any inline-C body may include:

```c
/* runtime/tur_result.h -- emitted by the compiler */
#include <stdbool.h>
#include <stdint.h>

void *tur_ok_int  (int64_t v);
void *tur_err_int (int64_t e);
void *tur_ok_ptr  (void   *v);
void *tur_err_ptr (void   *e);

void *tur_some_int(int64_t v);
void *tur_some_ptr(void   *v);
void *tur_none    (void);
```

Implementations live in the compiler runtime support library (TBD: same
TU as the closure dispatch helpers; see emit_module.c). They `malloc`
the canonical result / option struct using the same layout
`stdlib/result.tur` and `stdlib/option.tur` build via `make-struct`, so
any drift in either is caught by the compiler.

**Layout guarantee**: a static_assert in the runtime cross-checks the
struct layout against the compiler-emitted layout for `make-struct`'d
results / options. If the layout ever changes, every spice that uses
the helpers gets the new layout for free; if the static_assert fires
during compiler build, the layout drift is caught before spices break.

### Use from inline-C

```turmeric
(defn midi-in-new [api : cstr] : result<MidiIn, int>
  ```c
  #include <rtmidi/rtmidi_c.h>
  #include "tur_result.h"
  rtmidi_api_t api_val = (rtmidi_api_t)(__parse_midi_api(api));
  RtMidiInPtr mi = rtmidi_in_create(api_val, "Turmeric", 100);
  if (mi != NULL && mi->ok) return tur_ok_ptr(mi);
  if (mi) rtmidi_in_free(mi);
  return tur_err_int(-1);
  ```)
```

No hand-rolled struct. Return type is the real `result<MidiIn, int>`.
Callers consume with stdlib `ok?` / `ok-val` / `err-val` as for any
other result.

---

## Architecture

```
src/compiler/
  types.h                -- add TY_CFNPTR + CFnPtrInfo
  types.c                -- ty_equal etc. cases for TY_CFNPTR
  parse_types.c          -- parse (c-fn [A...] B) and (c-fn [A...] -> B)
  elab_fns.c             -- conversion site: defn -> c-fn upcast
  elab_call.c            -- call-through-c-fnptr lowering (already implicit)
  emit_module.c          -- typedef emission per c-fnptr signature
  runtime/
    tur_result.c         -- tur_ok_*, tur_err_*, tur_some_*, tur_none implementations
    tur_result.h         -- public header (emitted into build sysroot)

stdlib/
  result.tur             -- gains stable C-ABI exports for ok/err? (or runtime is the surface)
  option.tur             -- same for some/none
  docs/inline-c-results.md (new guide)

docs/guides/
  inline-c-results-guide.md (new) -- worked example, layout invariants

tests/fixtures/
  cfnptr-captureless-ok/        -- captureless defn passes as (c-fn ...)
  cfnptr-capturing-error/       -- capturing defn at c-fn site is a type error
  cfnptr-arity-error/           -- wrong arity at c-fn site is a type error
  cfnptr-roundtrip-c/           -- call C lib that takes a (c-fn ...) -> works
  inline-c-result-builder/      -- spice-shape: inline-C constructor returns
                                   result<Handle, int>; test ok? / ok-val
```

---

## Implementation Phases

- [ ] **TC0** -- Surface plan to the user with this document; confirm
  scope (especially: include `extern-fn` for already-extern-c functions,
  or v2?). No code changes yet.

- [ ] **TC1** -- `TY_CFNPTR` introduction (types only). Add the kind,
  carry the signature, plumb `ty_equal` / pretty-printer. No parser
  change yet; only the internal type. Cite: `src/compiler/types.h`,
  `src/compiler/types.c`.

- [ ] **TC2** -- Parser support for `(c-fn [A...] B)` /
  `(c-fn [A...] -> B)` in type-expression position. Cite:
  `src/compiler/parse_types.c` (or wherever the existing `(fn ...)`
  type form parses). One fixture: parse + pretty-print round-trip.

- [ ] **TC3** -- Elaborator: captureless-defn -> c-fnptr conversion.
  Use the existing capture-set machinery from `elab_fns.c:2798` /
  `2901`. Diagnostics for capturing-closure and signature-mismatch
  errors (TUR-E0NNN range; assign IDs). Three fixtures:
  `cfnptr-captureless-ok`, `cfnptr-capturing-error`,
  `cfnptr-arity-error`.

- [ ] **TC4** -- Codegen: emit per-signature typedefs and lower
  c-fnptr call sites as direct calls. Cite: `src/compiler/emit_*.c`.
  Fixture: `cfnptr-roundtrip-c` -- a captureless Turmeric defn passed
  as a `(c-fn ...)` to a tiny C shim that invokes it; verify return
  value.

- [ ] **TC5** -- Runtime helpers: `tur_ok_int`, `tur_err_int`,
  `tur_ok_ptr`, `tur_err_ptr`, `tur_some_int`, `tur_some_ptr`,
  `tur_none`. Header in build sysroot. Static_assert against
  `stdlib/result.tur` / `stdlib/option.tur` layout. Cite:
  `src/compiler/runtime/`, `src/compiler/emit_module.c`. Fixture:
  `inline-c-result-builder`.

- [ ] **TC6** -- Documentation: `docs/guides/inline-c-results-guide.md`
  with a worked rtmidi-shaped example, plus updates to the opaques
  guide cross-referencing both new patterns.

- [ ] **TC7** -- Update the audit and the CLAUDE.md gate: the audit's
  S1 and S3 sections move from "blocked on language pre-work" to "ready
  to mechanically apply". The CLAUDE.md rule's "if no real type exists,
  stop and report" stays in force; this plan lands the missing real
  types.

- [ ] **TC8** -- Validate by piloting on **one** spice
  (recommendation: rtmidi, since it has all three audit classes in a
  single small spice). Land a clean rtmidi v0.2.0 with `MidiIn` /
  `MidiOut` / `MidiCallback` opaques and `result<MidiIn, int>` returns
  using `tur_ok_ptr` / `tur_err_int` from inline-C. This is the
  exemplar PR other spice migrations will mirror.

- [ ] **TC9** -- Open the spice migration umbrella tracking issue /
  doc and unblock the audit's per-spice fix-up work.

---

## Design Notes

### Why a separate keyword, not a flag on `(fn ...)`

A `:c-abi` flag on `(fn ...)` was considered. Rejected because:

- The existing `(fn ...)` type is *already* overloaded between bare and
  fat representations. Adding a third sub-variant via a flag makes the
  type-equality story even worse.
- The conversion rules differ at the *type* level (which closures may
  upcast), so the kinds should be distinct so the elaborator can hang
  rules off the kind.
- `(c-fn ...)` reads at a glance as "this is the C-ABI form"; a flag
  on `(fn ...)` requires reading the modifier and remembering what it
  means.

### Why ship runtime helpers, not extern-c the existing `ok`/`err`

Considered: mark `stdlib/result.tur`'s `ok` / `err` as `extern-c
"tur_result_ok_int"` etc. Rejected for v1 because the existing closures
work for B-1 onwards is friendly to extern-c only with monomorphised
entries, and the migration of `ok<T>` to a parametric extern surface is
its own design problem. v1 ships fixed-shape runtime helpers; later
work can collapse them into the `extern-c` mechanism if desired.

### Why layout cross-checking via static_assert

If the layout of `result<T,E>` / `option<T>` ever changes (e.g.,
shrinking from `int64_t` slots, adding a tag-discriminant byte), the
inline-C helpers and the `make-struct` codegen must stay in sync.
static_assert in the runtime support TU is the cheapest way to make
drift a build error rather than a silent corruption.

### Why pilot on rtmidi (not tourist)

rtmidi has:
- One S1 site (`midi-in-set-callback`)
- Two S2 handle types (`MidiIn`, `MidiOut`)
- Two S3 inline-C constructors (`midi-in-new`, `midi-out-new`)
- No downstream callers (vendored snapshots under `plot/...` don't
  count)
- A small enough surface to validate end-to-end in one session

tourist is the highest-impact migration but is tangled with the
separate `docs/reported/tourist-middleware-takes-req-not-ctx.md`
reshape -- it should land *after* the rtmidi pilot proves the pattern
and *with* the middleware reshape, not on its own.

---

## Risks and Open Questions

1. **`extern-fn` design surface**. Should v1 ship `(c-fn ...)` only as a
   *type* with captureless-defn-conversion as the constructor, or
   should it also ship `(extern-fn name : (c-fn [A...] B))` as a way
   to declare an existing C symbol at that type? The latter is needed
   for "pass an arbitrary C lib function as a callback". Defer to v2
   unless there is a load-bearing use case in the spice audit (there
   isn't; the 5 S1 sites all consume callbacks defined in Turmeric).

2. **Generic `result<T, E>` from inline-C**. v1 ships
   `tur_ok_int`/`tur_ok_ptr` covering the audit's needs. Some future
   spice may want `result<MyStruct, MyErr>` where `MyStruct` /
   `MyErr` are user-defined types. Out of scope; document the
   limitation in the inline-C-results guide.

3. **`tur_some` for `option<T>` with `T = :void`**. `option<:void>` is
   a degenerate "flag" type. Skip from v1 helpers; use a `:bool` for
   the rare case where you'd want it.

4. **Header distribution**. The emitted `tur_result.h` needs to land
   somewhere spice build trees can include it. Coordinate with the
   manifest-driven build descent (already in main per CLAUDE.md) to
   add a `:cflags` / `:include` exposure for the runtime header.

5. **Existing `__ok` / `__err` patterns in rtmidi (and any other spice
   that copied them)**. Removed during TC8 pilot. Spices currently
   using the pattern get the cleanup as part of their own
   v0.2.0 migration, not pre-emptively.

6. **Diagnostic IDs**. Two new errors (capturing-closure-at-c-fn,
   c-fn-signature-mismatch). Assign IDs from the next free TUR-E0NNN
   slot; coordinate with the error-code registry.

---

## Files to Change

| File | Change |
|---|---|
| `src/compiler/types.h` | Add `TY_CFNPTR` kind + `CFnPtrInfo` |
| `src/compiler/types.c` | `ty_equal`, pretty-printer cases for `TY_CFNPTR` |
| `src/compiler/parse_types.c` (or current type-parser file) | Parse `(c-fn [A...] B)` and `(c-fn [A...] -> B)` |
| `src/compiler/elab_fns.c` | Captureless-defn -> c-fnptr conversion; diagnostics |
| `src/compiler/elab_call.c` | c-fnptr call sites (already implicit; verify) |
| `src/compiler/emit_module.c` (or current codegen file) | Per-signature typedef emission |
| `src/compiler/runtime/tur_result.c` (new) | `tur_ok_*`, `tur_err_*`, `tur_some_*`, `tur_none` |
| `src/compiler/runtime/tur_result.h` (new) | Public header for inline-C consumers |
| `src/compiler/runtime/CMakeLists.txt` | Install the header into the build sysroot |
| `stdlib/result.tur` | Add module-doc note pointing at the inline-C helpers |
| `stdlib/option.tur` | Same |
| `docs/guides/inline-c-results-guide.md` (new) | Worked rtmidi-shaped example |
| `docs/guides/opaques-guide.md` | Cross-ref the inline-C-results guide |
| `tests/fixtures/cfnptr-captureless-ok/` (new) | TC3/TC4 fixture |
| `tests/fixtures/cfnptr-capturing-error/` (new) | TC3 fixture |
| `tests/fixtures/cfnptr-arity-error/` (new) | TC3 fixture |
| `tests/fixtures/cfnptr-roundtrip-c/` (new) | TC4 fixture (runs against a tiny C shim) |
| `tests/fixtures/inline-c-result-builder/` (new) | TC5 fixture |
| `CLAUDE.md` | After landing: remove "blocked on language pre-work" caveat from the rule (the gate stays; the workaround footnote goes away). |

---

## Phase Status

| Phase | Title | Status |
|---|---|---|
| TC0 | Confirm scope with user | Done |
| TC1 | `TY_CFNPTR` kind + plumbing | Done |
| TC2 | Parser for `(c-fn [A...] B)` | Done |
| TC3 | Elaborator: defn -> c-fnptr conversion + diagnostics | Done |
| TC4 | Codegen: typedef emission + call lowering | Done |
| TC5 | Runtime: `tur_ok_*` / `tur_err_*` / `tur_some_*` / `tur_none` + header | Pending |
| TC6 | Documentation: guide + opaques cross-ref | Pending |
| TC7 | Audit / CLAUDE.md updates | Pending |
| TC8 | Pilot on rtmidi (exemplar v0.2.0) | Pending (callback site already uses `(c-fn ...)`; `midi-in-new` still returns `ptr<void>`) |
| TC9 | Umbrella tracking for per-spice migration | Pending |

---

## Cross-references

- `docs/reported/spices-int-stand-in-audit-2026-06-14.md` -- the audit
  that drives this work; ~50 sites across 19 spices.
- `docs/reported/typed-c-abi-function-pointers.md` -- S1 language gap.
- `docs/archive/history/no-stdlib-result-builder-for-inline-c.md` -- S3 stdlib
  gap.
- `docs/reported/tourist-middleware-takes-req-not-ctx.md` -- adjacent
  finding; tourist migration depends on both this plan AND that reshape.
- `docs/upcoming/v1/tourist-session-middleware-plan.md` -- the original
  trigger; blocked on both this plan and the tourist middleware
  reshape.
- `docs/archive/history/closure-representation-unification-plan.md` and
  `docs/archive/history/closure-first-class-type-plan.md` -- adjacent
  prior work; closed the boxed-vs-bare crash class but did not add a
  typed C-ABI surface.
- `CLAUDE.md` -- "No Lazy `:int` Stand-Ins -- STRICT RULE" (2026-06-14).
