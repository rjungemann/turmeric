# Storing a closure in a `defdata` or `defstruct` field: `:fn` segfaults, `:ptr<void>` works

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

## Root cause, and it is wider than `defdata`

**A `defstruct` field is affected too.** The title says `defdata`; the probe
below says otherwise:

```turmeric
(defstruct Box [run : fn])
(defn mk [n : int] : Box (let [ln n] (Box (fn [] (let [_ ln] (* ln 2))))))
;; calling (.run b) -> Segmentation fault
```

That matters because `[run : fn #fx{Effect}]` is the **capability-struct**
pattern, which several fixtures use. They all pass -- and every one of them
stores a *non-capturing* lambda (`(fn [s] (perform (Emit s)))`). The whole
in-tree corpus sits on the working side of this, which is why it was never
noticed.

**The mechanism, from the emitted C.** Forcing a `:fn` field emits a bare
function-pointer call:

```c
int64_t (*__call_head)() = (int64_t (*)())(intptr_t)(th);
... (((int64_t (*)(void))(intptr_t)__call_head)());
```

while the opaque-carrier field emits the fat-closure dispatch -- load the thunk
from slot 0, pass the env as the receiver:

```c
((*( tur_thunk_int64_t_t *)((void*)(intptr_t)(th)))((void*)(intptr_t)(th)));
```

So the *more precise* annotation selects the *less capable* calling convention.

**Why.** `TY_FN` carries a `boxed` flag that means exactly "this is a closure
value (fat box), not a bare function reference" (`types.h:693`). The ascription
that forces the field -- `(:: th (fn [] S))` -- builds a **fresh** `TY_FN` from
the source annotation, and a freshly constructed `TY_FN` has `boxed = false`
(`types.c:1375`). Nothing re-boxes it, so the call goes bare. The neighbouring
comment at `emit_expr.c` already records the underlying hole: the `boxed` flag
"is absent from both the `TY_FN` mangle and `type_eq`".

**Stale comment worth fixing while in there:** `types.h:701` still says of
`boxed` that "B-0 only plumbs the bit; nothing sets it true yet". It is set in
about ten places and read widely --
[closure-first-class-type-plan](../archive/history/closure-first-class-type-plan.md)
is marked COMPLETE (B-0..B-4 shipped) and the comment was never updated.

## Investigated 2026-08-26: it is a documented gap in a PRIOR fix

This is not an unknown bug. `elab_structs.c:1326` carries the fix for
[capturing-closure-in-struct-field-segv](../archive/history/capturing-closure-in-struct-field-segv.md),
which marks a `(fn ...)` field **boxed** so every read steers to the fat
dispatch -- precisely the crash described above. Its bound is the problem:

```c
/* Bound to arity 1..4: ... A nullary or >4-arg fn field stays on the
 * pre-existing thin path. */
if (t && t->kind == TY_FN && !t->as.fn.boxed &&
    t->as.fn.arity >= 1 && t->as.fn.arity <= 4) {
```

So a **nullary** fn field -- a thunk, which is exactly what a lazy stream or a
`with-*` bracket wants -- is *explicitly excluded* and left on the path that
segfaults. **`>4` is excluded too and carries the same latent crash**, untested
because nothing in the tree has a 5-arg fn field.

The bound's stated reason is that the store shim needs arity >= 1 and the read
dispatch covers N <= 4. Both halves turn out to exist for 0: `__tur_fatshim0`
(store) and `TUR_APPLY0_T` (read).

## Lowering the bound is NOT sufficient -- tried it

The obvious fix is to drop both floors to 0 (`elab_structs.c`'s boxing test and
`elab_call.c`'s `inner_arity < 1` skip). **Done, built, and it changes nothing**:
all three repros still segfault, and the suite stays 2698/0. Reverted rather
than shipped -- it is an unmotivated behaviour change that silently adds drop
glue and makes such structs move-only, with no demonstrated benefit.

The reason it does not help is the real finding:

## A nullary `:fn` field has NO working invocation form

| spelling | result |
|---|---|
| `(.run b)` | **reads** the field -- types as `(fn [] : ?)`, not a call |
| `((.run b))` | calls it, but the result types as `?` |
| `((:: (.run b) (fn [] int)))` | typechecks -- and **SIGSEGVs** on a capturing closure |

The direct-call form the boxing machinery targets (`(.run em "x")` for arity
>= 1) does not exist at arity 0, because with no arguments there is nothing to
distinguish a call from a read. So the only spelling that typechecks is the
ascription -- which rebuilds a fresh unboxed `TY_FN` and takes the thin path,
whatever the field is marked.

**That makes this elaboration work on the read-then-call path, not a bound
change.** Two things have to happen together: `(.run b)` / `((.run b))` on a
nullary fn field needs a resolved result type, and that path has to preserve
the fat representation instead of reconstructing an unboxed one.

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
2. **Make `:fn` fields work, or reject them.** Working means the ascription
   that forces the field must preserve boxedness rather than rebuilding an
   unboxed `TY_FN` -- which is the `boxed`-through-mangle-and-`type_eq` hole
   above, so it is type-system work, not a codegen patch.

   **The cheap interim is a diagnostic**, and after the investigation above it
   is the recommended next step rather than a fallback. The rule is now precise:
   reject the store of a CAPTURING closure into a `:fn` field whose arity is
   **outside the boxed range** -- 0, or > 4 -- because those are exactly the
   fields left on the thin path. Arity 1..4 already works and must keep
   working, so a blanket rejection would be wrong.

   Elaboration can already tell the two apart: the shim loop in `elab_call.c`
   distinguishes a bare `TY_FN` from a capturing closure today. The message
   should name the `defopaque :ptr<void>` route. This breaks nothing in the
   tree -- every in-tree `:fn` field stores a captureless lambda, which is why
   the crash was never hit.
3. **Document the working route.** Nothing says how to store a callback in an
   ADT; the answer is currently "read `Goal`'s declaration in `logic.tur`".

Direction 2 is the one that matters. Note this interacts with CLAUDE.md's
"No lazy `:int` stand-ins" rule: it tells authors to spell out function types
rather than erase them, and in a `defdata` field doing exactly that is the
option that crashes.
