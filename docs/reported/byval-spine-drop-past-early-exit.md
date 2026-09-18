# A by-value ADT local's spine drop is a trailing free, and two exits never reach it

**Severity: medium.** A linear leak on the shape the accumulator loop is
normally written in: a tail-recursive function that builds a list per turn
leaked the whole spine every turn (2 boxes x N, measured flat: 200 at 100
turns, 800 at 400), in both dialects, on both by-value spine paths.

**Status: PARTIALLY FIXED 2026-09-18, two passes.** The scalar-signature case
-- every parameter and the result a non-pointer scalar, which is the
accumulator loop -- is closed on both faces, on both spine paths, and (second
pass) on `emit_tail`'s value-returning exit and for the boxed fn-field drop
too. What remains open is the non-scalar-signature loop (residue 1) and a
separate finding about call-initialised fn-field locals; both at the end.

Found while executing
[any-widen-stored-in-an-adt-field-has-no-owner](any-widen-stored-in-an-adt-field-has-no-owner.md),
whose second pass had attributed this leak to
[byvalue-recursive-adt-boxes-are-never-freed](byvalue-recursive-adt-boxes-are-never-freed.md)
"as a tail call jumping past the trailing drop". Both halves of that were
wrong: that report's residues are a callee hand-off and `:copy`, neither of
which is this; and the drop was not jumped past -- it was never emitted.

## Repro

Face i, a tail self-call (plain Turmeric, the direct recursive field):

```turmeric
(defdata Lst [] (Cons [hd : int tl : Lst]) (Nil))
(defn drive [i : int n : int acc : int] : int
  (if (= i n) acc
    (let [xs (Cons 1 (Cons 2 (Nil)))]
      (drive (+ i 1) n (+ acc (match xs (Cons h t) h (Nil) 0))))))
(defn main [] : int (println (drive 0 100 0)) 0)
```

```
SUMMARY: AddressSanitizer: 4800 byte(s) leaked in 200 allocation(s).   ;; 100 turns
SUMMARY: AddressSanitizer: 19200 byte(s) leaked in 800 allocation(s).  ;; 400 turns
```

Face ii, an explicit `return` (same leak, 200 at 100 calls):

```turmeric
(defn one [i : int] : int
  (let [xs (Cons i (Cons 2 (Nil)))]
    (return (match xs (Cons h t) h (Nil) 0))))
```

The same two programs over `(Cons [hd : any tl : any])` -- the Saffron shape --
leak identically through the other glue path. Make the recursive call non-tail
(`(+ (match ...) (drive ...))`) and face i is clean; that is the whole bisect.

## Root cause

`drop_localowned_<T>(&xs)` is a TRAILING free, emitted by `emit_let_value`
after the let body (`emit_expr.c`, the single emission site). Two paths never
get there, and neither had been given the drop:

- **Face i.** A tail-position `let` in a function with a marked self-tail-call
  is emitted INLINE by `emit_tail`'s `tco_let_simple` arm (`emit_fns.c`), not
  through `emit_let_value`. That arm repeats the `any` drop bookkeeping --
  pushes the drop statements onto `any_scope_drops`, the channel the back-edge
  and the `return` site fire -- and its comment says why: "no trailing drop is
  emitted: emit_tail always ends in a `return` or a back-edge `goto`". The
  spine drop was never repeated there. The emitted `drive()` contains no drop
  call at all.
- **Face ii.** In `emit_let_value` the spine drop was collected inside
  `if (!body_has_return_or_throw)`, so a body with a `return` collected none.
  The `any` drop is collected UNGUARDED for exactly this reason (its comment:
  "they are trailing-only frees, so a body with an early exit gets none and
  leaks -- the status quo this rule is closing"), and
  `tests/fixtures/any-widen-drop-past-early-exit` closed both faces for `any`
  locals. The by-value spine locals were never given the same.

The sibling report calls the glue `drop_recspine_<T>`; that name is stale. Both
the direct recursive field and the `:any` field glue through
`drop_localowned_<T>` (`adt_def_has_localowned_glue`).

## What was fixed

The spine drop rides the same channel, at both sites, mirroring the `any` drop
exactly: collected outside the gate in `emit_let_value`, pushed onto
`any_scope_drops` before the body, emitted trailing on both fall-through paths;
pushed in `emit_tail`'s inline arm beside the `any` pushes.

**The gate is the load-bearing part, and it is one the `any` drop never
needed.** At a back-edge the channel fires AFTER the argument temporaries are
assigned to the parameters; at a `return`, after the value is hoisted to a temp
(`emit_stmt.c`). So the words that could still reach a spine freed on the way
out are exactly the parameters and the result. For an `any` local that is
harmless -- `(cast a Pt)` is a copy. For a by-value ADT it is not: a match
binder `t` out of `(Cons h t)` is a pointer INTO the spine, and a loop that
threads it through an `any` parameter would read a freed cell on the next
turn.

Hence `EmitCtx.current_fn_spine_drop_safe`, decided once per FnDef from the
SIGNATURE: every parameter and the result must be nil/bool/int/float or a
fixed-width kind -- not cstr, not ptr (deliberately narrower than
`type_is_atomic_scalar`, mirroring the frame-box family's `_result_safe`).
Reset to false in the copied handler/body sub-contexts (`emit_effects.c`) and
around a CPS term (`emit_cps_ir.c`), so the default is the status-quo leak,
never a free. Ungated, the trailing frees on fall-through still run: that is
the free the ungated path always emitted.

Pinned by `tests/fixtures/byval-spine-drop-past-early-exit` (both faces, both
glue paths, leak-clean, no marker) and
`tests/fixtures/byval-spine-drop-refused-nonscalar-param` (an `any` parameter
receiving a spine binder each turn: the gate must refuse, and it carries
`known-leak` naming this report because that refusal IS residue 1 below -- a
wrongly admitted push would show there as a use-after-free, which a marker
does not excuse).

**A trap the first fixture draft fell into, worth one paragraph.** Reading
`xs` through a callee that takes the ADT by value (`(rhead xs)`, `[xs : Rec]`)
MOVES it, so `drops_local_owned` is never set and there is no drop for this
fix to fire -- that is the sibling report's Residue 1, and the draft leaked all
1400 boxes with the fix in place. The inline `match` is what makes the local
droppable. The two reports meet exactly here: a local that is both built and
read in one scope is this report's; one handed to a by-value callee is the
sibling's.

## Residues -- second pass, 2026-09-18

Of the three residues the first pass listed, two are closed and the third was
partly wrong. Each was probed under ASan before anything was built.

1. **Non-scalar signatures -- OPEN, unchanged.** A loop whose parameters or
   result include an `any`, an ADT, a cstr or a pointer keeps the leak, because
   the gate cannot tell a binder that aliases the spine from a value that does
   not. Closing it needs the borrow/own distinction for by-value ADT arguments
   that the sibling report's fix direction 1 describes -- the same
   `nonretain_ptr_param_mask` family question, asked of the back-edge
   arguments. `byval-spine-drop-refused-nonscalar-param` pins the refusal.

2. **The value-returning default path inside `emit_tail` -- FIXED.** A
   tail-position `let` in a TCO'd function whose body ends in a plain VALUE
   reached the one exit out of `emit_tail` that never fired the channel, and
   everything the inline arm had pushed was popped unfired: the final turn of
   every accumulator loop that ends in a value leaked its locals -- measured 2
   boxes for a spine local, one box for an `any` local. The value is now
   hoisted to a temp (it may read what is about to be dropped), the channel
   fires, then the return -- `emit_stmt.c`'s return arm, repeated; the void
   branch discards first and needs no hoist. Where no return ctype is recorded
   the channel is left unfired rather than guessed at (`__auto_type` silently
   costs the JIT). This closed the `any` drops' identical gap in passing.

3. **The other drops behind the gate -- one fixed, one was never there, one
   unverified.**
   - **Boxed fn-field: FIXED**, same channel, same gate; it had both faces.
     Pinned in `byval-owned-local-drop-past-value-tail` with a struct LITERAL
     initialiser -- see the finding below for why that qualifier matters.
   - **Closure env: not a residue.** The first pass listed it on the strength
     of the elaborator's comment ("the closure-env and catch-box frees have the
     same shape and the same hole"). Probed: a capturing closure bound in a
     tail-position `let` of a TCO'd loop is already clean. Whatever that
     comment is describing, it is not this trailing free.
   - **Catch box: unverified.** Not probed; it sits behind the same gate and
     would take the same treatment, with its own argument about what a
     back-edge argument can alias.

### New finding: a fn-field local initialised from a CALL is never dropped at all

Not an early-exit hole -- it leaks with no early exit anywhere:

```turmeric
(defstruct H [f : (fn [int] int)])
(defn mk [k : int] : H (make-struct H (fn [x : int] : int (+ x k))))
(defn h [i : int n : int] : int
  (if (= i n) 0
    (let [hh (mk i)]
      (+ ((.f hh) 1) (h (+ i 1) n)))))     ;; NOT a tail call
```

```
SUMMARY: AddressSanitizer: 2400 byte(s) leaked in 100 allocation(s).
```

`drop_fnfields_tur_adt_H` is emitted as glue and never called: `drops_fn_fields`
is not set for `hh`. Replace `(mk i)` with the literal
`(make-struct H (fn ...))` and the same program is clean, in every position.
So the flag's guards refuse a call-initialised local -- which of the three
(`binding_moved_during_init`, `is_moved`, `is_binding_consumed`) is not yet
established, and the fix belongs to the fn-field auto-drop, not to this
report's channel. Recorded here because it is what stopped the first fn-field
probe from exercising the channel at all, and it is the shape a factory
function produces.
