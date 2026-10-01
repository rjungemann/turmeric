# A class method's `(Option a)` result was untyped inside a constrained generic

**Severity: high.** A checker hole over a representation seam:

```turmeric
(defclass Co [a] (co [x : a] : (Option a)))
(definstance Co [float] (co [x : float] (some x)))
(defn g2 [a] [(Co a)] [x : a] : a (unwrap-or (co x) x))
;; error: function 'unwrap-or' arg 1: expected (Option A), got (? ?)
(defn g3 [a] [(Co a)] [x : a] : a (match (co x) (Some q) q (None) x))
;; error: match: arm types are incompatible -- expected int, got tyvar
(defn g6 [x : float] : cstr (co x))     ; accepted, then invalid C
```

Found 2026-10-01 widening the type fuzzer's rank-2 class crossing to a method
with an applied result. **RESOLVED 2026-10-01.**

## Mechanism

Three layers, each of which hid the next.

1. **Elaboration.** On an abstract receiver, `elab_method_call` takes the
   result from the carrier *representative*. For an applied result that is
   `type_from_kind(TY_APP)`, the def-less `(? ?)`, which unifies with anything.
   The existing rule (`let-bound-class-method-result-in-constrained-generic-truncates`)
   only covered a bare `a -> a` result. The concrete `g6` case was the
   return-check hole in `applied-type-annotations-unchecked.md`.
2. **The dict slot.** Each instance's impl returns its own by-value monomorph
   (`tur_adt_Option__float`, `tur_adt_Option__cstr`). The dict clone cast every
   slot to the representative's struct. That read the float instance's payload
   from `rdx` instead of `xmm0`, and the result was then spilled into an
   `int64_t`.
3. **The base carrier clone.** It calls the representative statically, gets
   the by-value aggregate, and hands it to `(Option a)` consumers, which the
   carrier spells as a word. The let binder was an invalid initializer, the
   match scrutinee "aggregate value used where an integer was expected", and an
   argument tripped the representation cross-check (ICE at `arg-bridge`).

## Fix

- `elab_typeclasses.c`: on an abstract receiver of a kind-* class, a result
  that mentions the class variable inside an application is the class's
  declared result with the variable substituted by the receiver's type.
- `dict_slot_result_is_word_scalar` (`emit_stmt.c`) covers such a result when
  the impl returns a by-value aggregate. The slot holds a `__dictwrap_*` that
  boxes it into the carrier word. The struct field, the dispatch cast and
  `emit_call_dispatches_word_result` all agree.
- The call hoist boxes an open-typed representative result where it is
  produced (`call_open_class_result_boxed`, escaping, with a region note). Both
  by-value-producer predicates report such a call, and a word-returning dict
  dispatch, as a word.
- `call_arg_spill_type` spills what the producer emits when the stated type
  is still open.

## Verified

`tests/fixtures/class-var-applied-result-in-constrained-generic` covers:

- `Option`, `Result` and `Pair` results;
- float, float32, bool, cstr and a by-value struct;
- unwrap-or, let, match, tail and a nested-generic route;
- rank-2 calls through the dict.

It runs compiled and `--interpret` with 0 float-lint findings. On the
baseline it does not type-check (`got (? ?)`).

## Not covered

The boxes the slot wrapper and the production bridge allocate are not freed,
which is the carrier's usual ownership gap for sum boxes. `run.sh` does not
leak-check emitted programs.
