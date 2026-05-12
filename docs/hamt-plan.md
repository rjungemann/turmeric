# Persistent Collections — HAMT (Phases P1–P4)

**Status:** Planned (v2). Prerequisites: Phase 15 (Typeclasses — `Eq`/`Hash` dispatch needed for key comparison), Phase 19 (Algebraic effects — `@ {Unsafe}` tracks internal raw memory use in `hamt.c`). See [archive/hamt-feasibility.md](archive/hamt-feasibility.md) for feasibility analysis and design decisions.

**Key design decisions:**
- **Separate translation units:** `src/hamt.{c,h}` are compiled independently. Unused HAMT code is stripped by the linker automatically (`-dead_strip` on macOS, `-Wl,--gc-sections` on Linux) — no emit-phase gating required in v1.
- **Memory model:** ref-counting (Phase P1 v1); GC integration deferred until GC lands.
- **Node width:** 5 bits (32 slots) — balanced trie depth vs. bitmap size.
- **Hash function:** SipHash (cryptographic) or xxHash (speed); decision finalized in Phase P1.
- **Collision handling:** linked list in v1; upgrade to HAMT-of-HAMTs if benchmarks warrant.
- **Transient mode (mutable buffer → flush to immutable):** deferred to Phase P4.
- **Compiler lowering:** the emit-phase `immutable map → HAMT` lowering pass is Phase P3; Phases P1–P2 are pure library work with no compiler changes.

| Phase | Goal | Exit Criterion |
|---|---|---|
| **P1** | Core HAMT C implementation | `hamt.{c,h}` complete; all unit fixtures pass; ref-count memory-safe under AddressSanitizer |
| **P2** | Lisp bindings (`stdlib/hamt.tur`) | `hamt/new`, `hamt/set`, `hamt/get`, `hamt/del`, `hamt/has?`, `hamt/count`, `hamt/keys`, `hamt/vals`, `hamt/merge` all working; fixture suite passes |
| **P3** | Compiler lowering pass | Elaborator lowers immutable `map` literals and `assoc`/`dissoc` calls to HAMT when the map is annotated `^persistent` or inferred immutable; existing mutable-map code unchanged |
| **P4** | Optimization and tooling | Transient mode (`hamt/transient`, `hamt/persistent!`), `hamt/dump` visualization, benchmarks vs. mutable hash table |

---

## Prerequisites (Phases P1–P4)

- [x] Phase 15 (Typeclasses v1) is stable — needed for `Eq` and `Hash` dispatch for key comparison.
  - Confirmed: Phase 15 is complete; `Eq` typeclass is available for user-defined key types.
- [ ] Decide `Hash` typeclass design: mirror `Eq` / `Ord` pattern (`(defclass Hash [a] (hash [x : a] : uint64))`) vs. a free function per type.
  - Pending decision. Recommendation: `Hash` typeclass alongside `Eq` in `stdlib/typeclass.tur`; primitive instances use `tur_siphash13` or `tur_xxhash64` under the hood.
- [ ] Decide hash function: `tur_siphash13` (cryptographic, DoS-resistant) vs. `tur_xxhash64` (speed-optimised, non-cryptographic).
  - Pending decision. Document in a comment at the top of `src/hamt.c` once resolved.
- [ ] Confirm `^persistent` annotation syntax does not conflict with any reserved annotation in `src/reader.{c,h}`.
  - Pending check. Annotations beginning with `^` are reader-level metadata; confirm `persistent` is not taken.
- [ ] Define `hamt` surface type name: `hamt<K V>` vs. `persistent-map<K V>` vs. `pmap<K V>`.
  - Pending decision. Recommendation: `hamt<K V>` as the canonical type; `persistent-map` as a stdlib alias.
- [ ] Confirm linker flag policy for dead-code stripping: `-dead_strip` (macOS), `-Wl,--gc-sections` with `-ffunction-sections -fdata-sections` (Linux).
  - Confirmed per [hamt-feasibility.md §Dead Code / Inclusion Strategy](archive/hamt-feasibility.md). These flags are emitted by `tur build` when targeting those platforms; no emit-phase gating required.
- [ ] Define whether Phase P3 (`^persistent` lowering) requires Phase 19 effect-row infrastructure (for `@ {Unsafe}` propagation through `hamt.c`).
  - Pending decision. Recommendation: Phase P1–P2 are pure library work; Phase P3 (lowering pass) can begin once Phase 19 Section A (effect-row surface syntax) is stable. Phase P4 (transient mode) has no dependency on effect rows.

---

## Phase P1 — Core HAMT C Implementation

**Goal:** Implement a standalone, ref-counted HAMT in C99 with a minimal public API. No compiler integration yet; this is pure library code.

**New files** — `src/hamt.{c,h}`
- [ ] Define node types: `HAMT_NODE_BITMAP` (sparse, ≤ 32 entries), `HAMT_NODE_ARRAY` (dense, = 32 entries), `HAMT_NODE_COLLISION` (same-hash key list).
- [ ] Define `HamtNode` tagged union and `Hamt` root struct `{ HamtNode *root; uint32_t count; }`.
- [ ] Implement `hamt_new(void) → Hamt *` — allocate empty HAMT.
- [ ] Implement `hamt_set(Hamt *m, uint64_t hash, void *key, void *val) → Hamt *` — structural sharing; returns new root.
- [ ] Implement `hamt_del(Hamt *m, uint64_t hash, void *key) → Hamt *` — returns new root or same root if key absent.
- [ ] Implement `hamt_has(Hamt *m, uint64_t hash, void *key) → bool`.
- [ ] Implement `hamt_get(Hamt *m, uint64_t hash, void *key) → void *` — returns `NULL` if absent.
- [ ] Implement `hamt_count(Hamt *m) → uint32_t`.
- [ ] Implement `hamt_free(Hamt *m)` — decrement root ref-count; free nodes with zero refs.
- [ ] Implement `hamt_node_retain(HamtNode *n)` / `hamt_node_release(HamtNode *n)` — ref-counting helpers.
- [ ] Implement `hamt_merge(Hamt *a, Hamt *b) → Hamt *` — `b` wins on collision.
- [ ] Implement `hamt_iter_init` / `hamt_iter_next` — in-order iteration over key/value pairs.
- [ ] Choose and integrate hash function: `tur_siphash13` or `tur_xxhash64`; document decision in a comment at top of `hamt.c`.
- [ ] Add `hamt_dump(Hamt *m, FILE *out)` — pretty-print node tree for debugging.

**Memory safety requirements**
- [ ] All node allocations go through `hamt_alloc` / `hamt_free_node` (wrappers around `malloc`/`free`); no `malloc` calls outside these wrappers.
- [ ] All unit tests run clean under AddressSanitizer (`-fsanitize=address`) and Valgrind.
- [ ] No memory leaked between `hamt_new` and `hamt_free` even when intermediate `hamt_set`/`hamt_del` results are discarded.

**Fixtures** — `tests/fixtures/hamt/`
- [ ] `hamt-basic.tur` — `hamt/new`, `hamt/set`, `hamt/get`, `hamt/has?`, `hamt/count` round-trip.
- [ ] `hamt-sharing.tur` — two maps share structure after `hamt/set`; modifying one does not affect the other.
- [ ] `hamt-delete.tur` — `hamt/del` on present and absent keys; count decrements correctly.
- [ ] `hamt-collision.tur` — insert multiple keys with the same hash; all retrievable.
- [ ] `hamt-iteration.tur` — iterate all key/value pairs; no duplicates, no omissions.
- [ ] `hamt-merge.tur` — merge two disjoint maps; merge with overlapping keys (last writer wins).
- [ ] `hamt-large.tur` — insert 10 000 unique keys; verify count and random-sample lookups.
- [ ] `hamt-memory.tur` — ASan clean: insert, snapshot, mutate snapshot, free both versions.

**Exit criterion:** all C unit tests pass; fixtures pass; ASan/Valgrind clean; `hamt_dump` produces legible output.

---

## Phase P2 — Lisp Bindings

**Goal:** Wrap `src/hamt.{c,h}` in a Turmeric stdlib module so Lisp code can use HAMTs directly. No compiler lowering yet — this is an explicit API.

**New file** — `stdlib/hamt.tur`
- [ ] `(hamt/new) → hamt` — create empty persistent map.
- [ ] `(hamt/set m key val) → hamt` — insert/update; returns new map.
- [ ] `(hamt/get m key) → (option T)` — lookup; returns `none` if absent.
- [ ] `(hamt/get-or m key default) → T` — lookup with fallback.
- [ ] `(hamt/del m key) → hamt` — delete; returns new map (same map if key absent).
- [ ] `(hamt/has? m key) → bool` — membership test.
- [ ] `(hamt/count m) → int` — number of key/value pairs.
- [ ] `(hamt/keys m) → (vec T)` — all keys as a vector.
- [ ] `(hamt/vals m) → (vec T)` — all values as a vector.
- [ ] `(hamt/entries m) → (vec (pair K V))` — all key/value pairs.
- [ ] `(hamt/merge a b) → hamt` — merge; `b` wins on collision.
- [ ] `(hamt/merge-with f a b) → hamt` — merge with combiner function for collisions.
- [ ] `(hamt/map f m) → hamt` — map function over values; returns new map.
- [ ] `(hamt/filter f m) → hamt` — filter by predicate on value; returns new map.
- [ ] `(hamt/reduce f init m) → T` — fold over key/value pairs.
- [ ] `(hamt/from-vec pairs) → hamt` — construct from `(vec (pair K V))`.
- [ ] `(hamt/to-vec m) → (vec (pair K V))` — destructure to association list.

**Typeclass instances**
- [ ] `Show` instance for `hamt` — `(show m)` returns `"{key1: val1, key2: val2, ...}"`.
- [ ] `Eq` instance for `hamt` — two maps are equal if they have the same key/value pairs.

**Fixtures** — `tests/fixtures/hamt/`
- [ ] `hamt-lisp-basic.tur` — basic API round-trip from Turmeric code.
- [ ] `hamt-lisp-snapshot.tur` — take snapshot with `hamt/set`, verify original unchanged.
- [ ] `hamt-lisp-map-filter.tur` — `hamt/map`, `hamt/filter`, `hamt/reduce`.
- [ ] `hamt-lisp-merge-with.tur` — `hamt/merge-with` combiner function.
- [ ] `hamt-lisp-show.tur` — `Show` instance produces expected string.
- [ ] `hamt-lisp-eq.tur` — `Eq` instance: equal and unequal maps.
- [ ] `hamt-lisp-from-to-vec.tur` — `hamt/from-vec` / `hamt/to-vec` round-trip.
- [ ] Codegen snapshots: HAMT function calls lower to `hamt_*` C calls; no unexpected overhead.

**Exit criterion:** all stdlib functions are usable from Turmeric; typeclass instances work; fixture suite passes; codegen snapshots stable.

---

## Phase P3 — Compiler Lowering Pass

**Goal:** Allow the elaborator to automatically lower immutable `map` literals and persistent map operations to HAMT when beneficial, without requiring explicit `hamt/` namespace calls.

**Elaborator changes** — `src/elab.{c,h}`
- [ ] Recognize `^persistent` annotation on `def`/`let` bindings: `(def ^persistent m {:a 1 :b 2})` lowers the map literal to `hamt/from-vec`.
- [ ] Recognize `(assoc m k v)` on a `^persistent`-typed binding: lowers to `hamt/set`.
- [ ] Recognize `(dissoc m k)` on a `^persistent`-typed binding: lowers to `hamt/del`.
- [ ] Recognize `(get m k)` on a `^persistent`-typed binding: lowers to `hamt/get`.
- [ ] Recognize `(count m)` on a `^persistent`-typed binding: lowers to `hamt/count`.
- [ ] Propagate `^persistent` through `let` bindings and function return types.
- [ ] Emit a type-mismatch diagnostic when a `^persistent` map is passed to a function expecting a mutable map (and vice versa).

**Emit changes** — `src/emit.{c,h}`
- [ ] When `needs_hamt` flag is set on the `Emit` context (set by the P3 lowering pass), include `hamt.h` in the emitted C header block.
- [ ] `needs_hamt` is set on first encounter of any HAMT-lowered form; existing code that never uses `^persistent` is unaffected.

**Linker flag policy** — `src/emit.{c,h}` (already decided; document here)
- [ ] No `-lhamt` needed (HAMT is compiled into the binary as part of `src/`).
- [ ] Dead-code stripping: on macOS add `-dead_strip`; on Linux add `-Wl,--gc-sections` (with `-ffunction-sections -fdata-sections` on object files). These flags are emitted by `tur build` automatically when targeting those platforms.

**Fixtures** — `tests/fixtures/hamt/`
- [ ] `hamt-lowering-basic.tur` — `(def ^persistent m {:a 1})` followed by `assoc`/`get` lowers to HAMT calls.
- [ ] `hamt-lowering-propagate.tur` — `^persistent` propagates through `let` chains.
- [ ] `hamt-lowering-mutable-unchanged.tur` — ordinary (mutable) map operations are unaffected by the lowering pass.
- [ ] Negative: `hamt-lowering-type-mismatch.tur` — passing `^persistent` map to mutable-map function emits TUR-E00XX.
- [ ] Codegen snapshots: `assoc`/`dissoc`/`get` on `^persistent` map lower to `hamt_set`/`hamt_del`/`hamt_get`.

**Exit criterion:** `^persistent` annotation triggers HAMT lowering end-to-end; non-annotated code is unaffected; codegen snapshots stable; type-mismatch diagnostic fires correctly.

---

## Phase P4 — Optimization and Tooling

**Goal:** Add transient mutation mode for batch construction, visualization tooling, and performance benchmarks.

**Transient mode** — `src/hamt.{c,h}` + `stdlib/hamt.tur`
- [ ] Define `HamtTransient` struct: mutable wrapper around a `HamtNode *` root; carries an owner token to prevent concurrent mutation.
- [ ] Implement `hamt_transient(Hamt *m) → HamtTransient *` — fork a transient from an immutable map; marks all nodes as owned-by-transient.
- [ ] Implement `hamt_transient_set(HamtTransient *t, uint64_t hash, void *key, void *val)` — mutates in-place if node is owned; copies otherwise.
- [ ] Implement `hamt_transient_del(HamtTransient *t, uint64_t hash, void *key)` — mutates in-place if owned.
- [ ] Implement `hamt_persistent(HamtTransient *t) → Hamt *` — seal transient into immutable map; invalidates `t`.
- [ ] Lisp API: `(hamt/transient m) → hamt-transient`, `(hamt/transient-set! t k v)`, `(hamt/transient-del! t k)`, `(hamt/persistent! t) → hamt`.

**Visualization** — `src/hamt.{c,h}`
- [ ] Extend `hamt_dump` to produce DOT format for Graphviz: `hamt_dump_dot(Hamt *m, FILE *out)`.
- [ ] Add `(hamt/dump m)` Lisp form that emits DOT to stderr (debug builds only).

**Benchmarks** — `tests/benchmarks/hamt/`
- [ ] `hamt-bench-insert.tur` — 100 000 sequential inserts; compare vs. mutable hash table baseline; target < 3× overhead.
- [ ] `hamt-bench-lookup.tur` — 100 000 lookups after 100 000 inserts; target O(1) average.
- [ ] `hamt-bench-snapshot.tur` — 1 000 snapshots (fork + 1 insert each); total time vs. deep-copy baseline.
- [ ] `hamt-bench-transient.tur` — bulk-build 100 000 entries via transient then seal; compare vs. sequential `hamt/set`.

**Fixtures** — `tests/fixtures/hamt/`
- [ ] `hamt-transient-basic.tur` — fork transient, mutate, seal; verify result correct.
- [ ] `hamt-transient-isolation.tur` — original map unchanged after transient mutations.
- [ ] `hamt-transient-invalidated.tur` — using a sealed transient panics or errors.

**Exit criterion:** transient mode is correct and faster than sequential `hamt/set` for bulk construction; benchmarks documented; `hamt_dump_dot` produces valid DOT output.
