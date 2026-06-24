# Plan: Type System C-ABI Surface -- Follow-ups (TC5-TC9)

> **Status:** Pending. Split out from the original
> `type-system-c-abi-plan.md` (now archived) after TC1-TC4 landed.
> **Last Updated:** 2026-06-23
> **Type:** Compiler runtime + stdlib + spice migration
> **Predecessor:** `docs/archive/type-system-c-abi-plan.md` (TC0-TC4: typed
>   `(c-fn [A...] B)` surface; landed and consumed by
>   `tourist-session/store.tur` and `rtmidi/in.tur`'s callback site).
> **Triggers:**
>   - `docs/reported/spices-int-stand-in-audit-2026-06-14.md` (the audit)
>   - `docs/archive/history/no-stdlib-result-builder-for-inline-c.md` (S3
>     stdlib gap; still open at the inline-C constructor sites)
> **CLAUDE.md gate:** "No Lazy `:int` Stand-Ins -- STRICT RULE"
>   (2026-06-14). The predecessor plan landed the typed-callback half of
>   the unblock; this plan lands the inline-C `result<T,E>` builder half
>   and the rtmidi exemplar pilot that proves both ends.

---

## Why a separate plan

The predecessor plan bundled two language/stdlib changes (typed C-ABI
function pointers AND an inline-C result builder) plus the rtmidi pilot
plus the audit/umbrella tracking work. The function-pointer half (TC1-TC4)
landed cleanly and is now in use. The remaining work is:

- An additional set of runtime helpers and a header surface for inline-C
  result/option construction (TC5).
- The documentation, audit edits, and spice pilot that depend on TC5
  (TC6-TC9).

Splitting these out makes the still-pending work easier to track and to
schedule against other v1-track items without dragging a "Partial" plan
forward indefinitely.

---

## Background recap

The codebase already emits per-TU `static __attribute__((unused))` helpers
in `src/compiler/emit_module.c:4856-4899`:

- `tur_box_some(int64_t) -> int64_t`
- `tur_is_some(int64_t) -> bool`
- `tur_opt_value(int64_t) -> int64_t`
- `tur_box_ok(int64_t) -> int64_t`
- `tur_box_err(int64_t) -> int64_t`
- `tur_is_ok(int64_t) -> bool`
- `tur_ok_value(int64_t) -> int64_t`
- `tur_err_value(int64_t) -> int64_t`

These match the `tur_option_t` / `tur_result_box_t` byte-layout used by
`stdlib/result.tur` and `stdlib/option.tur` and are already exercised by
existing inline-C blocks. So the predecessor plan's "no stdlib result
builder for inline-C" framing is **partially obsolete**: a constructor
can already `return tur_box_ok((int64_t)(intptr_t)handle);` and have it
flow into stdlib `ok?` / `ok-val`.

What is still missing -- and what TC5 here delivers -- is:

1. A **typed naming discipline** (`_int` vs `_ptr` suffixes) so the
   inline-C author does not have to remember which cast direction is
   correct for pointer payloads.
2. A **stable header** (`tur_result.h`) callable by spices whose inline-C
   bodies do not want to depend on preamble-emitted statics being in
   scope.
3. The **rtmidi pilot** (TC8) actually migrated to use them, so the
   audit's S2/S3 fix-up work becomes a copy-the-pattern exercise.

---

## Scope

### In scope

- Decide between two implementation shapes for the runtime helpers (see
  Open Questions below) and land one.
- `tur_ok_int` / `tur_err_int` / `tur_ok_ptr` / `tur_err_ptr` /
  `tur_some_int` / `tur_some_ptr` / `tur_none` available to inline-C with
  return type `void *` (or documented `int64_t` if we keep the existing
  carrier shape).
- A guide page (`docs/guides/inline-c-results-guide.md`) with a worked
  rtmidi-shaped example.
- Update the audit (`docs/reported/spices-int-stand-in-audit-2026-06-14.md`)
  and CLAUDE.md so the "blocked on language pre-work" caveat on S1/S3 is
  removed -- the gate stays; the workaround footnote goes.
- Pilot on rtmidi: `MidiIn` / `MidiOut` / `MidiCallback` opaques,
  `result<MidiIn, int>` / `result<MidiOut, int>` returns from inline-C
  constructors, cut a clean rtmidi v0.2.0 release. This is the exemplar
  for downstream spice migrations.
- Open the umbrella tracking doc for per-spice migration of the audit's
  remaining sites.

### Out of scope

| Future enhancement | Description |
|---|---|
| Generic `result<T, E>` from inline-C with arbitrary user types | v1 ships only the `_int`/`_ptr` monomorphised entries. |
| `tur_some` for `option<:void>` | Degenerate flag-only option; use `:bool`. |
| `extern-fn` declaration form | Already deferred from the predecessor; not load-bearing. |
| Per-spice migration of the audit's remaining 19 sites | Tracked by the umbrella doc TC9 opens; this plan only delivers the rtmidi exemplar. |
| Replacing the per-TU `static` preamble helpers | Even if we ship a shared header, the legacy `tur_box_*` names stay valid -- they are how every existing inline-C block in the tree calls into the layout, and rewriting them is not load-bearing. |

---

## Implementation phases

- [x] **TC5** -- Runtime helpers: `tur_ok_int`, `tur_err_int`,
  `tur_ok_ptr`, `tur_err_ptr`, `tur_some_int`, `tur_some_ptr`,
  `tur_none`. Decide on shape (open question 1 below). Static_assert
  cross-check against `stdlib/result.tur` / `stdlib/option.tur` layout.
  Fixture: `tests/fixtures/inline-c-result-builder/`.
  **Landed (shape a):** the seven helpers are emitted alongside the
  existing `tur_box_*` statics in `emit_closure_fat_runtime`
  (`src/compiler/emit_module.c`). `_int` variants take an `int64_t`
  payload directly; `_ptr` variants take a `void *` and widen it through
  `intptr_t` so the inline-C author never hand-writes the cast;
  `tur_none()` is the function companion to the `TUR_NONE` macro. A
  `_Static_assert` pair pins `tur_option_t` / `tur_result_box_t` to the
  `2*int64`/`3*int64` byte layout shared with stdlib. Fixture exercises
  all seven against the stdlib accessors (`ok?`/`err?`/`ok-val`/`err-val`/
  `some?`/`unwrap`).

- [x] **TC6** -- Documentation:
  `docs/guides/inline-c-results-guide.md` with a worked rtmidi-shaped
  example; cross-ref from `docs/guides/opaques-guide.md`.
  **Landed:** new guide covers the full helper table (typed
  `tur_ok_ptr`/`tur_err_int`/`tur_some_ptr`/`tur_none` plus the
  carrier-level `tur_box_*` and the inspectors), a worked rtmidi
  `MidiIn` `(Result MidiIn int)` constructor, the `_Static_assert`
  layout guard, the two anti-patterns it replaces, and the
  `_int`/`_ptr`-only limitation. Cross-referenced from
  `opaques-guide.md` (inline-C/ABI section + See also) and registered in
  the guides `README.md`. Also reconciled the stale
  `c-integration-guide.md` "Returning result/option" section, which
  documented non-existent `tur_ok`/`tur_err`/`tur_some` names and falsely
  claimed there were no `_int`/`_ptr` variants -- it now uses the real
  builder names and points at the new guide.

- [x] **TC7** -- Update the audit
  (`docs/reported/spices-int-stand-in-audit-2026-06-14.md`) and the
  CLAUDE.md gate footnote: S1 and S3 move from "blocked on language
  pre-work" to "ready to mechanically apply." The strict rule itself
  stays in force.
  **Landed (state had moved on):** the audit was already archived to
  `docs/archive/` with every S1/S3 row marked *fixed*, and the
  "blocked on language pre-work" caveat the predecessor plan slated for
  CLAUDE.md was never actually added there -- so there was no literal
  footnote to delete. Honoured the intent instead: (1) added an
  "Inline-C constructor half unblocked (2026-06-24, TC5/TC6)" note to the
  archived audit's Phase 3, recording that with the typed builders +
  guide landed, every remaining S3-style "build a result inside inline-C"
  site is ready to mechanically apply, not blocked; (2) added a paragraph
  to the CLAUDE.md "No Lazy `:int` Stand-Ins" rule stating that returning
  `result`/`option` from inline-C is first-class (no `:int` escape
  hatch), pointing at the new guide. The strict rule stays in force.

- [x] **TC8** -- Pilot on rtmidi. Land a clean rtmidi v0.2.0 with
  `MidiIn` / `MidiOut` / `MidiCallback` opaques and
  `result<MidiIn, int>` / `result<MidiOut, int>` returns from inline-C
  constructors. Remove the `__ok` / `__err` hand-rolled struct patterns
  in rtmidi.
  **Landed (2026-06-24):** rtmidi v0.2.0 in `../turmeric-spices/spices/rtmidi/`
  no longer hand-rolls the canonical Result struct. `midi-in-new` /
  `midi-out-new` return real `(Result MidiIn int)` / `(Result MidiOut int)`
  built with `tur_ok_ptr` / `tur_err_int` directly in inline-C; the
  fallible operations in `rtmidi/in` / `rtmidi/out` (open / open-virtual /
  send) return `(Result int int)` the same way. The `__ok` / `__err`
  helpers and the `midi-in-of` / `midi-out-of` extractors are gone --
  callers read the handle out with stdlib `ok-val`. `build.tur` :exports
  drops the extractor names; README quick start uses the typed shape.
  `MidiCallback` did not need a new opaque -- the callback parameter is
  already typed `(c-fn [float ptr<const-u8> usize ptr<void>] void)`,
  which is what the predecessor plan's typed-c-fn (TC1-TC4) shipped for
  exactly this site.
  Pre-flight `git grep midi-in-new midi-out-new` across `turmeric/` +
  `turmeric-spices/`: only the rtmidi spice itself, its README, and the
  generated `web/dist/client/doc-names.json` reference these constructors
  -- no out-of-tree consumer needed updating.

- [x] **TC9** -- Open the spice migration umbrella tracking doc and
  unblock the audit's per-spice fix-up work. List each audit site, link
  to the rtmidi pilot PR as the exemplar.
  **Landed:** `docs/upcoming/spice-int-stand-in-migration-umbrella-plan.md`
  tracks the 13 remaining spices (27 source files) that still hand-roll
  `__ok` / `__err` helpers (identified by `grep -l 'defn __ok\|defn __err'`
  across `../turmeric-spices/spices/*/src/`), gives the per-spice
  migration recipe, and points at rtmidi v0.2.0 as the exemplar. Each
  row is independent; v1 does not gate on the list closing.

---

## Open questions

1. **Helper shape** -- two reasonable options:
   - **(a) Extend the preamble.** Add `tur_ok_int` / `tur_err_int` /
     `tur_ok_ptr` / `tur_err_ptr` / `tur_some_int` / `tur_some_ptr` /
     `tur_none` as additional static helpers in `emit_module.c`'s
     preamble alongside the existing `tur_box_*` helpers. Smallest diff;
     no build-sysroot plumbing; every spice gets the helpers
     automatically; the `tur_result.h` header (if we want one) is a thin
     documentation-only file that just declares what's already in scope.
   - **(b) Real runtime library + installed header.** Build
     `src/compiler/runtime/tur_result.{c,h}`, install the header into the
     build sysroot, and have inline-C bodies `#include "tur_result.h"`.
     Matches the predecessor plan's letter; requires CMakeLists changes
     and sysroot exposure for spices.
   - Recommendation: ship (a) first, treat (b) as a possible follow-up
     if a real need (e.g., out-of-tree spices that don't go through the
     `tur` driver) materialises. The predecessor plan's preference for
     (b) was written before the per-TU preamble helpers were as fleshed
     out as they are now.

2. **Generic `result<T, E>` from inline-C**. v1 ships
   `tur_ok_int` / `tur_ok_ptr` covering the audit's needs. Some future
   spice may want `result<MyStruct, MyErr>` where `MyStruct` / `MyErr`
   are user-defined types. Out of scope; document the limitation in the
   inline-C-results guide.

3. **Diagnostic IDs**. None new in TC5-TC9 (the c-fn-related
   diagnostics already shipped with TC3).

4. **rtmidi v0.2.0 ABI break**. The pilot's signature changes
   (`midi-in-new : cstr -> result<MidiIn, int>` instead of
   `:ptr<void>`) are a breaking change for any in-tree consumer. Audit
   `git grep midi-in-new` before TC8 starts and update callers in the
   same PR.

---

## Files likely to change

| File | Change |
|---|---|
| `src/compiler/emit_module.c` | (shape a) extend preamble with new `tur_*_int` / `tur_*_ptr` / `tur_none` helpers; (shape b) reference the installed header instead. |
| `src/compiler/runtime/tur_result.{c,h}` (new, shape b only) | Implementation + public header. |
| `src/compiler/runtime/CMakeLists.txt` (shape b only) | Install header into build sysroot. |
| `tests/fixtures/inline-c-result-builder/` (new) | TC5 fixture; spice-shape constructor returning `result<Handle, int>`. |
| `docs/guides/inline-c-results-guide.md` (new) | TC6 worked example. |
| `docs/guides/opaques-guide.md` | TC6 cross-ref. |
| `docs/reported/spices-int-stand-in-audit-2026-06-14.md` | TC7 status updates on S1/S3 rows. |
| `CLAUDE.md` | TC7 footnote removal on the strict-rule "blocked on language pre-work" caveat. |
| `../turmeric-spices/spices/rtmidi/` | TC8 pilot: opaques, `result<...>` constructors, drop hand-rolled `__ok`/`__err`. |
| `docs/upcoming/spice-int-stand-in-migration-umbrella-plan.md` (new) | TC9 per-spice tracker. |

---

## Cross-references

- `docs/archive/type-system-c-abi-plan.md` -- predecessor plan
  (TC0-TC4 landed).
- `docs/reported/spices-int-stand-in-audit-2026-06-14.md` -- audit
  driving this work.
- `docs/archive/history/no-stdlib-result-builder-for-inline-c.md` -- S3
  stdlib gap (now partially mitigated by the existing per-TU preamble
  helpers; TC5 hardens the surface).
- `CLAUDE.md` -- "No Lazy `:int` Stand-Ins -- STRICT RULE" (2026-06-14).
