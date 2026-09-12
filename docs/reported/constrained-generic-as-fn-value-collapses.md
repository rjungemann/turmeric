# A constrained generic passed as a function value collapses to one instance

**Severity: medium-high** -- a **silent wrong answer**, but only for a shape
that is currently worked around by duplicating code, so nothing in the tree
depends on it today.

**Status:** open. Found 2026-09-12 trying to remove the duplicated fold in
`crdt/ormap` (see "Why it matters").

## Repro

```turmeric
(defclass JS [a] (j [x : a y : a] : a))
(defopaque Gmax :int)
(definstance JS [Gmax] (j [x y] (if (< (:: x int) (:: y int)) y x)))   ;; max
(defopaque Gsum :int)
(definstance JS [Gsum] (j [x y] (:: (+ (:: x int) (:: y int)) Gsum)))  ;; sum

(defstruct M [V] [e : int])

(defn vjoin [V] [(JS V)] [x : int y : int] : int   ;; constrained
  (:: (j (:: x V) (:: y V)) int))

(defn fold [f : (fn [int int] int) a : int b : int] : int (f a b))

(defn mjoin [V] [(JS V)] [a : (M V) b : (M V)] : int
  (fold vjoin (.e a) (.e b)))                      ;; vjoin as a VALUE
```

`(mjoin .. Gmax)` and `(mjoin .. Gsum)` both answer `12`. Called directly
rather than through `fold`, `vjoin` is correct at both.

## Why it matters

This is what forces `crdt/ormap` to carry two nearly identical folds --
`__side-loop` taking the combine as an argument, and `__side-loop-j` calling
the class method directly. They differ in exactly one line. A constrained
generic that could be passed as a value would collapse them into one.

## How far it gets, and the exact remaining gap

Measured by attempting the fix, in case that saves the next attempt the
detour. Three layers have to line up; the first two can be made to work:

1. **The enclosing generic must specialize.** `mjoin`'s body has no
   class-method call of its own -- the constrained generic is an ARGUMENT, not
   the callee -- so `body_has_dispatch_on_app_tyvar` answers no. Following
   fn-typed arguments to their bodies fixes this. (Note both this probe and
   `emit_abi_scan_fn_values` strip only `EX_ASCRIBE`/`EX_FN_TO_FAT`, while a fn
   argument crossing into a poly slot is wrapped in `EX_POLY_WRAP` -- so the
   variable is invisible to both until that is stripped too.)

2. **The fn value must get per-instantiation clones.**
   `emit_abi_scan_fn_values` bails on `if (!abi_changes) continue;`, and two
   `defopaque` newtypes over int have the same ABI. Adding the same
   `instance_changes` question the direct-call path asks produces correct
   clones: `vjoin__spec__...` calling `__inst_JS_j_Gmax` and `...__h1` calling
   `__inst_JS_j_Gsum`. Verified in the emitted C.

3. **The poly WRAPPER must be per-clone -- this is the gap.** The
   `tur_poly_fn_t` literal names a single wrapper (`__poly_1626`) created at
   elaboration, whose body hardcodes one clone. Both `mjoin` specializations
   reference that one wrapper, so the correct clones from step 2 are never
   reached. With steps 1-2 applied the answer changes from `12 12` to `9 9` --
   a different wrong answer, not a fix.

   Closing it needs wrapper clones per inner specialization plus spec-aware
   naming in value position: `raw_name_for_binding` and `atom_var` both return
   the base name and consult no specialization.

Steps 1 and 2 were implemented and then **reverted** -- they are inert for
every existing fixture (zero snapshot churn, suite green) but fix nothing on
their own, and landing untestable half-machinery is worse than a clean gap.

## Fix direction

Intern a specialization for the wrapper binding alongside the inner one, and
make the poly-fn literal resolve the wrapper name through the enclosing
specialization.

## Fixture owed

The repro above, asserting `9` then `12`.
