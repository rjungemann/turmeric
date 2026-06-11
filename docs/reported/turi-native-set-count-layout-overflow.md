# `native_set_count`/`native_set_member` heap-overflow on a HAMT-backed Set

**Summary:** The interpreter's `set-*` native shims
(`native_set_count`/`native_set_member`/..., `src/main.c:5886+`) assume a Set is
laid out as `int64_t[2] = { ptr-to-sorted-array, count }` (the `EX_SET_LIT`
literal layout in `src/turi/eval.c:3433`). But `stdlib/set.tur` defines a Set as
`(defstruct Set [A] (hamt :ptr<void>))` -- a HAMT-backed set with a completely
different layout. When `set.tur` is loaded under the interpreter, `set-new`
returns the struct layout while `set-count`/`set-member?` read it as the
two-word array, producing an out-of-bounds heap read.

**Severity:** Medium (memory-safety bug, ASan heap-buffer-overflow), but
currently **latent**: the default `--interpret` path (`cmd_eval`) does **not**
preload `set.tur` (TI8.b/W1 deliberately excludes it), so a normal
`tur --interpret <file>` that uses sets fails earlier with `unbound variable:
set-new` instead. It bites whenever `set.tur` *is* loaded alongside the set
natives -- e.g. `wk_eval_fixture` (`src/main.c:6671`) preloads `set.tur`
(`:6736`) and registers the set natives (`:6489`), and any future prelude that
adds `set.tur` (W1b).

## Repro

```sh
cat > /tmp/set.tur <<'EOF'
(load "stdlib/typeclass-eq.tur")
(load "stdlib/typeclass-hash.tur")
(load "stdlib/hamt.tur")
(load "stdlib/set.tur")
(defn main [] : int (println (set-count (set-new))) 0)
EOF
ASAN_OPTIONS=detect_leaks=0 ./build/tur --interpret /tmp/set.tur
# => AddressSanitizer: heap-buffer-overflow in native_set_count (src/main.c)
```

(Observed while landing TI8.b/W1 via `typed/set-basic` once the prelude included
`set.tur`.)

## Root cause

`src/main.c:5881-5884`:

```c
/* Set represented as int64_t[2]: [0]=ptr to sorted int64_t array, [1]=count
 * (matches EX_SET_LIT interpreter layout in eval.c) */
```

vs `stdlib/set.tur:13`:

```turmeric
(defstruct Set [A] (hamt :ptr<void>))
```

`set-new` (`set.tur:24`) allocates the `Set` struct (one `hamt` pointer);
`native_set_count` (`src/main.c:5902`) dereferences `s[1]` as a `count` word
that does not exist, and `native_set_member` then indexes
`items[...]` off a bogus base pointer.

## Observed vs. expected

- **Observed:** heap-buffer-overflow READ in `native_set_count` /
  `native_set_member`.
- **Expected:** `set-count` returns the cardinality of the HAMT-backed set.

## Complication: the natives also serve `#set{}` literals (dual representation)

The same `set-count`/`set-member?` natives back the `#set{...}` literal, which
`EX_SET_LIT` lowers to the **int64[2] sorted-array** layout the natives expect.
So the natives are *correct* for `#set{}` literals and *wrong* for `set.tur`
HAMT-backed sets -- two incompatible runtime representations routed through one
native, with **no runtime tag to distinguish them**. A naive "repoint
`native_set_count` at the hamt" therefore breaks `#set{}` literals. This is the
crux of why W1b is a representation-unification problem and not a one-line fix:
`#set{}` literals, `set.tur` structs, the native int64-box, and `make-struct`
TuriStructs are four different in-memory shapes for the "same" value, and the
`native_*` shims silently assume one of them.

## Fix directions

1. **Unify the Set representation first**, then make the natives match it. Either
   route `#set{}` literals through the HAMT path too (so one layout exists), or
   tag set values so the natives can branch. Only then can `set.tur` join the W1
   prelude. Read the `hamt` field and call `tur_hamt_count`/`tur_hamt_get`
   (both exist: `src/runtime/hamt.c:852,914`).
2. **Or drop the `set-*` natives and let `set.tur` run** (its ops are
   HAMT-backed; if the hamt path is interpretable, no native is needed).
3. **Or register an override for `set-new` that produces the `int64_t[2]`
   layout the natives expect** -- but that diverges from the compiled ABI and is
   the wrong direction.

Direction 1 is preferred and is part of the W1b "reconcile the native-shim
layer" follow-up in
[docs/upcoming/v1/turi-interpreter-gap-closure-plan.md](../upcoming/v1/turi-interpreter-gap-closure-plan.md).
The same audit applies to the `native_ok`/`ok-val`/`result-map` (Result) and
hamt-invalidation shims, which have the analogous mismatch with `result.tur` /
`hamt.tur`.

## Validation

After a fix, the repro above prints `0` (empty set) under `--interpret` with
ASan clean, and `set.tur` can be added back to the W1 prelude without
regressing `typed/set-basic`.

## Status

Filed while executing TI8.b/W1. W1 sidesteps the bug by excluding `set.tur` from
the interpreter prelude; this report tracks the underlying native/module layout
mismatch so W1b can close it.
