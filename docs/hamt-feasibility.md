# HAMT Feasibility Analysis for Turmeric

A plan and feasibility analysis for implementing Persistent Hash Array Mapped Tries (HAMTs) in Turmeric (Lisp→C99 compiler).

## What are HAMTs?

Hash Array Mapped Trie — an immutable hash map with:
- O(1) average lookup/insert/delete
- O(log₃₂ n) worst-case (assuming 32-bit hash chunks)
- Structural sharing → O(1) copy/snapshot
- Memory overhead: ~1.5-2x vs mutable hash table

## Feasibility: HIGH ✅

| Aspect | Assessment | Notes |
|--------|------------|-------|
| **Language** | Good fit | C99 supports pointers, malloc, manual memory mgmt |
| **Immutability** | Natural | Turmeric already has `^mut` annotation — default immutable |
| **Memory model** | Works | Need GC or manual ref-counting for sharing |
| **Performance** | Acceptable | ~2-3x slower than mutable hash tables (sharing overhead) |
| **Complexity** | Medium | ~500-800 lines for full implementation |

## Implementation Plan

### Phase A: Core HAMT (2-3 days)
```
hamt.h      — Public API: hamtset, hamtdel, hamtset, hamtdel, hamthas, hamtget
hamt.c      — Internal: bit manipulation, node types, collision handling
  └── Node types: bitmap (sparse), array (dense), collision (hash clash)
```

### Phase B: Lisp Integration (1-2 days)
```
stdlib/     — Add hamt.tur wrapper module
src/        — Lowering pass: persist Lisp maps to HAMT when immutable
```

### Phase C: Optimization (optional)
- Transient mode (mutable buffer → flush to immutable)
- Cache-friendly node layout
- Custom allocator for nodes

## Memory Management Options

| Approach | Pros | Cons |
|----------|------|------|
| **Ref counting** | Simple, deterministic | Overhead on every operation |
| **GC (mark-sweep)** | Simpler API | Need full GC implementation |
| **Manual (Rust-style)** | Zero overhead | Burden on user |
| **Arena allocator** | Fast, simple | No free until arena reset |

**Recommendation:** Start with ref-counting (easiest), migrate to GC later if Turmeric adds GC.

## API Sketch (C99)

```c
typedef struct HamtNode HamtNode;
typedef struct { HamtNode* root; } Hamt;

Hamt* hamt_new(void);
Hamt* hamt_set(Hamt* map, uint64_t hash, void* key, void* value);
Hamt* hamt_del(Hamt* map, uint64_t hash, void* key);
bool  hamt_has(Hamt* map, uint64_t hash, void* key);
void* hamt_get(Hamt* map, uint64_t hash, void* key);
void  hamt_free(Hamt* map);  // Decref root
```

**Lisp binding:**
```lisp
(def persistent-map (hamt/new))
(def updated (hamt/set persistent-map :key "value"))  ; returns new map
```

## Design Decisions

| Consideration | Choice | Rationale |
|---------------|--------|-----------|
| **Hash function** | xxHash or SipHash | Fast, good distribution, minimal collisions |
| **Node width** | 5 bits (32 slots) | Balance between depth and bitmap size |
| **Collision handling** | Linked list | Simple; upgrade to HAMT-of-HAMTs if needed |
| **Memory layout** | Struct-of-arrays | Cache-friendly for iteration |

## Risk Assessment

| Risk | Mitigation |
|------|------------|
| **Memory bloat from sharing** | Profile early, add compaction pass |
| **C99 pointer overhead** | Use uintptr_t for tagged pointers where possible |
| **Debugging complexity** | Add visualization/dump functions |
| **Performance regression** | Benchmark against simple hash table baseline |

## Estimated Effort

| Task | Lines of Code | Time |
|------|---------------|------|
| Core HAMT (C) | 600-800 | 2-3 days |
| Lisp FFI bindings | 100-150 | 1 day |
| Tests | 150-200 | 1 day |
| Integration with compiler | 50-100 | 0.5-1 day |
| **Total** | **900-1250** | **4-6 days** |

## Recommendation

**Proceed.** HAMTs are a natural fit for:
- Immutable data orientation of functional languages
- C99's manual memory management
- Turmeric's existing immutability-by-default stance

**Start with:** Minimal C implementation + Lisp bindings. Validate with property-based tests (insert/get roundtrip, sharing verification).

**Defer:** Transient mode, custom allocators, GC integration until core is stable.
