# Matched boxed ADT / handler value not dropped (general owning-value teardown gap)

> **Archived 2026-07-19** as a known limitation (the report's own Resolution
> section, below, already settled this as documented memory-model behavior rather
> than a defect). Re-verified against the current tree under
> `cc -fsanitize=address,undefined`: the non-effect `Box` probe leaks 16 bytes,
> the effect-free `adt-recursive` Cons list leaks 24 bytes (and PASSES the suite,
> whose run phase is not leak-checked), and the `cps-backend-effect-under-match`
> fixture leaks its `Full` box (`ctor_Full <- route__cps`) identically on the
> shipping flag-off path -- confirming a bare non-`rc` heap ADT box is never
> auto-freed on any path, exactly as `docs/guides/gc-guide.md` documents. Making
> these free is a language-level memory-model decision for the owner, not a
> targeted fix. See also the sibling `cps-match-scrutinee-not-dropped` report,
> which describes the same 16-byte scrutinee box on the experimental flag-on path
> and is subsumed by this determination.

**Severity:** low (small, one-shot leaks near program end; the suite's run phase is
not leak-checked for these fixtures, so they pass). **NOT CPS-specific** -- see the
correction below. The original title said "on the CPS path"; that framing was
wrong and is kept only as the filename.

## Symptom

```sh
CC="cc -fsanitize=address,undefined -g" \
  ./build/tur build tests/fixtures/cps-backend-effect-under-match/input.tur -o /tmp/e
ASAN_OPTIONS=detect_leaks=1 /tmp/e
# => Direct leak of 16 byte(s): malloc <- ctor_Full <- route__cps <- run__cps
```

A boxed `Full` ADT is constructed (`(Full 7)`), passed to `pick`, matched to
extract its int, and the box is never freed. Output is correct; only the box leaks.

Second instance -- `defstruct-field-handler` / `-multi` / `fh-multi-effect-type`:
a first-class handler value (`(handler (Ask [] k) ...)`) is allocated via
`tur_handler_table_new(1)` (+ its `entries` array), stored in a `defstruct` field,
installed via `with-handler`, and never freed (16 + 40 bytes; `tur_handler_table_free`
is emitted but never called).

## Root cause -- CORRECTED (2026-07-19)

The earlier "the direct emitter applies the drop, the CPS path does not" premise is
**FALSE**, verified by a minimal NON-effect probe:

```turmeric
(defdata Box (Empty) (Full :int))
(defn pick [b : Box] : int (match b (Full v) (+ v 1) (Empty) 0))
(defn main [] : int (println (pick (Full 7))) 0)
```

This program has no effects, no CPS/DK lowering -- it goes through the ordinary
direct emitter -- and it leaks the **same 16 bytes** from `ctor_Full`. So the box
is not dropped on EITHER path: Turmeric simply does not insert a scope-exit drop
for a matched-and-consumed **non-linear** boxed ADT. Likewise a first-class handler
value is never freed on any path (handler values only ever existed in effect code;
pre-graduation the fiber path also `tur_handler_table_new`'d and never freed it).

So both leaks are a **general owning-value teardown gap in the ownership/drop
subsystem**, present regardless of the CPS/DK endgame. They were merely *surfaced*
by Stage G making these fixtures DK-lower end to end; they are NOT a regression
introduced by the CPS work, and the fix does not live in the CPS emitter.

Open question for the language owner: is a matched non-linear boxed ADT *meant* to
be dropped? If ADT values are owned (drop-on-scope-exit), this is a real bug in
`match` teardown (both emitters). If non-linear ADTs are intentionally
value/leak-semantics and only `^linear`/`^unique` ones are freed, then only the
handler-value leak (which has no ownership annotation path) is a genuine defect.

## Resolution (2026-07-19): this is the documented memory model, not a defect

A wider probe settles it. **A bare (non-`rc<T>`) heap-boxed sum ADT is never
auto-freed on ANY path -- this is Turmeric's documented memory model, not a bug in
`match` teardown, and not CPS-specific.** Evidence:

- The core, long-shipping `adt-recursive` fixture -- a plain
  `(defdata List (Nil) (Cons :int :List))` built and matched with no effects, no
  CPS -- leaks **24 bytes** (its `Cons` boxes) under ASan, and PASSES the suite.
- `docs/guides/gc-guide.md` states the model directly: *"Only values whose type is
  `rc<T>` participate in RC or GC. Everything else is stack-allocated,
  arena-allocated, or manually freed."* A boxed sum ADT with a heap payload that is
  not wrapped in `rc<T>` is a **non-`rc` heap value**: the compiler emits the
  `ctor_*` `malloc` but no automatic drop. Managed lifetime is opt-in via `rc<T>`
  (RC + last-use-elision drop) or the substructural (`^linear`/`^unique`) path.
- The suite stays green because the compiled-program **runtime** is intentionally
  NOT leak-checked -- only the compiler/codegen path (`tur build`/`emit-c`) runs
  under LeakSanitizer (CLAUDE.md leak policy; gc-guide "The interpreter is
  different" section). The run phase of a fixture leaking a bare ADT box is
  invisible to `bash tests/run.sh`.

So both leaks here -- the `Full` box and the handler-value `tur_handler_table` --
are the SAME accepted "non-`rc` heap value is not auto-freed" behavior, in the same
category as the known/accepted `interp-collections-never-freed` leak. They are
**working as designed for unmanaged ADTs**; the Stage-G endgame neither caused nor
worsened them (it only changed which emitter produces the identical `malloc`).

### If one ever wants these freed

It is a language-level memory-model change (make `defdata` boxes RC-managed by
default, or insert an ownership-tracked scope-exit drop for consumed non-linear
boxes), touching the **direct emitter's** drop pass and every boxed-ADT program's
codegen -- a broad, opt-in-today decision for the language owner, NOT a targeted CPS
fix. For the handler value specifically, the narrow version is: give `TY_HANDLER`
drop glue (`tur_handler_table_free`) driven from the scope-exit pass, matching how
`rc<T>` values already drop. Until such a decision, this report is best read as a
**known limitation** documenting the boundary, not an open defect blocking anything
(the fiber-effect-runtime deletion is complete and leak-neutral).
