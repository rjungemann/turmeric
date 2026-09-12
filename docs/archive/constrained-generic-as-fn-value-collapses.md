# A constrained generic passed as a function value collapses to one instance

**Severity: medium-high** -- a **silent wrong answer**, but only for a shape
that is currently worked around by duplicating code, so nothing in the tree
depends on it today.

**Status: RESOLVED** 2026-09-12. Found trying to remove the duplicated fold in
`crdt/ormap`; that duplication is now gone.

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

## Three layers, all now fixed

Measured by attempting the fix. Three layers had to line up:

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

3. **The poly WRAPPER must be per-clone.** The `tur_poly_fn_t` literal named a
   single wrapper created at elaboration, whose body hardcodes one callee, so
   both specializations reached the same instance. With only steps 1-2 the
   answer moved from `12 12` to `9 9` -- a different wrong answer.

   Fixed by emitting a wrapper variant per inner clone, on demand, and having
   the literal name the variant matching the enclosing specialization's type
   bindings. Bindings are the only way to tell the siblings apart: a fn-value
   clone shares its C signature with every other clone of the same generic.

## The first fix was name capture -- corrected the same day

The version first landed matched the caller's bindings to the callee's by tyvar
NAME. That makes a generic's meaning depend on the spelling of a bound
variable: alpha-renaming the callee's `[V]` to `[W]` silently stopped the
inheritance and every instantiation collapsed again, on both back ends.
`crdt/ormap` was one rename away from merging every map with the wrong join --
confirmed by doing it, which turned `test_ormap_join` red.

The key is the constraint's **CLASS**, which is what a dictionary is keyed on:
the caller holds one for class C, the callee needs one, pass it. A caller with
two constraints on the same class is genuinely ambiguous and is skipped rather
than guessed. `emit_translate_bindings_by_class` re-keys the caller's bindings
onto the callee's tyvars; the interpreter's capture uses `frame_lookup_dict`.

The fixture carries alpha-renamed rows on both back ends so this cannot
regress.

## A latent use-after-free this surfaced

`emit_abi_intern_spec` copies its `bindings` / `arg_types` arguments into the
new spec *after* growing `ctx->abi_specializations`. When a caller passes an
enclosing spec's own bindings -- which point INTO that array -- the realloc
leaves them dangling, and the copy is a heap-use-after-free (ASan caught it at
once). Latent until a caller actually interned while holding a spec's bindings;
the fn-value scan began doing exactly that. Both inputs are now copied locally
before the growth.

## Fixture

`tests/fixtures/constrained-generic-as-fn-value` asserts the two direct calls
(the control, correct throughout) and the two fn-value calls, so a regression
that repairs only the controls is not mistaken for a fix.

It carries a `requires.compiled` marker: the INTERPRETER still collapses the
fn-value case (`9 12 12 12`), recorded as a third symptom on
[turi-nested-class-method-call-picks-first-instance](../reported/turi-nested-class-method-call-picks-first-instance.md),
whose root cause -- the interpreter mints no per-instance specialization -- is
the same one.
