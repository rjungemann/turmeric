# Storing a closure in a `defdata` field: `:fn` segfaults, `:ptr<void>` works but warns

**Severity:** medium. A field type the compiler accepts and type-checks
miscompiles into a crash for the only case anyone would use it for. The working
alternative is undocumented and discoverable only by reading how `Goal` is
declared.

**Status:** OPEN. Root-caused with three probes below, not fixed. Found while
scoping the fix for
[logic-streams-are-strict](logic-streams-are-strict.md), which needs exactly
this -- a thunk stored in a `Stream` variant.

## 1. `:fn` field + capturing closure = SIGSEGV

```turmeric
(defdata S :copy (SNil) (SCons :int :S) (SInc :fn))

(defn force-one [s : S] : S
  (match s (SInc th) ((:: th (fn [] S))) _ s))

(defn delay-cons [n : int rest : S] : S
  (let [ln n
        lr rest]
    (SInc (fn [] (let [_ ln] (SCons ln lr))))))       ;; captures ln, lr

(defn main [] : int
  (let [lazy (delay-cons 7 (SNil))]
    (match (force-one lazy) (SCons v rest) (println v) _ (println -1))
    0))
```

```
Segmentation fault
```

A capturing lambda is a **fat closure** -- a `void *` to a closure record --
but `force-one`'s `(:: th (fn [] S))` ascription calls it as a bare function
pointer. The emitted C shows the value arriving unconverted:

```
note: expected 'int64_t' {aka 'long int'} but argument is of type 'void *'
 3383 | static int64_t ctor_SInc(int64_t _0) {
```

## 2. `:fn` field + NON-capturing closure "works"

The same program with `(SInc (fn [] (SCons 42 (SNil))))` prints `42`. A
non-capturing lambda lifts to a top-level `static int64_t __fn_NNNN()`, a bare
function pointer, which is what the call site assumes -- so it happens to be
right. It still warns (`int64_t (*)()` passed to `int64_t`).

**This is the worst combination for a user**: the shape you write first works,
and the shape you need crashes.

## 3. `:ptr<void>` field + capturing closure works

```turmeric
(defopaque Th :ptr<void>)
(defdata S :copy (SNil) (SCons :int :S) (SInc :Th))
;; construct: (SInc (:: (fn [] ...) :Th))
;; force:     ((:: th (fn [] S)))
```

Prints `7`. This is the route `stdlib/logic.tur` already takes for `Goal`
(`(defopaque Goal [A] :ptr<void>)` -- carrier is the fat-closure pointer), and
it is the only working way to put a closure in an ADT field today.

It still emits the same int-conversion warning at the ctor call.

## The warning is closure-specific

A plain pointer field with an ordinary pointer value is clean:

```turmeric
(defdata P :copy (PNil) (PSome :ptr<void> :int))
(defn mkptr [] : ptr<void> ```c static int x = 5; return (void*)&x; ```)
;; (PSome (mkptr) 3) -- no warnings, prints 3
```

Zero warnings. So the ordinary pointer path does insert the cast that the
closure path omits: `ctor_*` takes every field as `int64_t` (the ADT carrier
convention), and a closure value reaches it without the conversion an ordinary
pointer gets. Benign on LP64, not benign under `-Werror` or on a target where a
function pointer does not fit an `int64_t`.

## Fix directions

1. **Emit the cast on the closure path**, matching what ordinary pointer values
   already get. Fixes the warning in cases 2 and 3; does not fix case 1.
2. **Make `:fn` fields work, or reject them.** The call-through in case 1 has to
   dispatch a fat closure rather than assume a bare function pointer -- the same
   dispatch `apply-goal` performs. If that is not wanted, a `:fn` field should be
   a compile error naming the `defopaque :ptr<void>` route, because silently
   accepting it and crashing at runtime is the worst option and is what happens
   today.
3. **Document the working route.** Nothing says how to store a callback in an
   ADT; the answer is currently "read `Goal`'s declaration in `logic.tur`".

Direction 2 is the one that matters. Note this interacts with CLAUDE.md's
"No lazy `:int` stand-ins" rule: it tells authors to spell out function types
rather than erase them, and in a `defdata` field doing exactly that is the
option that crashes.
