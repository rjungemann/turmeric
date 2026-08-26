# Storing a closure in a `defdata` field: `:fn` segfaults, `:ptr<void>` works but warns

**Severity:** medium. A field type the compiler accepts and type-checks
miscompiles into a crash for the only case anyone would use it for. The working
alternative is undocumented and discoverable only by reading how `Goal` is
declared.

**Status:** PARTIALLY RESOLVED 2026-08-26. **Case 3 (the `:ptr<void>` route) is
fixed** -- the emitted C is clean and the lazy-stream work it blocked has
landed. **Case 1 (`:fn` field segfaults) is still OPEN**, and so is the warning
on case 2.

Kept in `reported/` for that reason. Found while scoping the fix for
[logic-streams-are-strict](logic-streams-are-strict.md), which needed exactly
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

It still emits the same int-conversion warning at the ctor call -- **and that
warning is a hard blocker, not cosmetic.** See below.

## The warning is closure-specific

A plain pointer field with an ordinary pointer value is clean:

```turmeric
(defdata P :copy (PNil) (PSome :ptr<void> :int))
(defn mkptr [] : ptr<void> ```c static int x = 5; return (void*)&x; ```)
;; (PSome (mkptr) 3) -- no warnings, prints 3
```

Zero warnings -- and note `ctor_PSome(void * _0, int64_t _1)`: a raw pointer
field gets a **pointer-typed parameter**, so no cast is needed at all.

**Corrected root cause.** An earlier revision of this report said `ctor_*`
takes every field as `int64_t` and the closure path is missing a cast. The
second half is right, the first is not: field parameters are typed per field,
and `int64_t` for an opaque field is *deliberate* -- an opaque "stays the int64
carrier" by design (`types.h:353`). So the defect is only the missing
conversion at the call, on the closure path.

And it is more precise than "missing cast": `emit_expr.c:~6280` **already
implements** it (`slot_is_i64 && arg_is_ptr` -> `(int64_t)(intptr_t)(...)`).
It does not fire for a closure because it can only determine the argument's C
type in three cases -- an already-cast string, a `(T *)(intptr_t)` string, or an
`EX_VAR` whose spec type resolves. A closure argument is none of those; it
emits as a compiler temp (`void *__t82 = __t80;`) whose type the logic cannot
see. **The fix is to widen that type determination, not to add a new cast** --
and the narrowness looks deliberate (a comment there warns that a blanket cast
would paper over a mis-selected monomorph), so it wants a snapshot diff.

**Corrected severity: this BLOCKS work, it is not cosmetic.** `tests/run.sh`
gates on emitted-C pointer/integer warnings. The lazy-stream fix for
[logic-streams-are-strict](logic-streams-are-strict.md) is written, works, and
turns **9 shipped fixtures red** on this warning alone -- so it is reverted and
parked in [../upcoming/lazy-streams-plan.md](../upcoming/lazy-streams-plan.md)
until this is fixed. Any future ADT that wants to hold a callback hits the same
wall.

## Fix directions

1. ~~**Widen the argument-type determination in `emit_expr.c`**~~ -- **DONE
   2026-08-26.** Two predicates at the ctor-argument seam were extended, and
   between them the existing pointer-to-carrier cast now fires for case 3:

   - `field_is_carrier` now also holds for an **opaque** field, not only a
     `TY_TYVAR` one. An opaque "has NO fields; it is a named int64_t carrier"
     with its declared base erased (`types.h:353`), so a `(defopaque Th
     :ptr<void>)` field lowers to `int64_t` exactly like a tyvar one.
   - `is_ptr_like` now also holds when the argument, with any ascription
     stripped, is an `EX_CLOSURE`. Ascribed to an opaque a closure *resolves*
     to that opaque (a carrier) while still *lowering* to a pointer, so only
     the expression form reveals the straddle.

   **Zero snapshot drift** across all 147 `expected.c` fixtures, and the suite
   is 2694/0 with `TUR_SANITIZER_GATE=1`. Mutation-verified: reverting it alone
   turns the `logic-*` fixtures red again on the cc-warning gate.

   Case 1 is untouched -- a `:fn` field still segfaults on a capturing closure.
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
