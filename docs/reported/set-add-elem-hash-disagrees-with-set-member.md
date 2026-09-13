# `set-add-elem__` adds elements that `set-member?` cannot find

**Severity: medium.** A silent wrong answer at the API level: the element IS in
the set (`set-count` counts it) but every membership test says `false`. Nothing
crashes, so a caller mixing the two spellings gets a set that quietly behaves
as if empty.

**Status:** open. Found 2026-09-11 building `spices/crdt`'s G-Set on
crdt-spice-plan C2.  **Root-caused 2026-09-13** (below); the fix is in type
inference, not in `stdlib/set.tur`, and it is guarded by a recorded regression.
Severity is HIGHER than filed: the `cstr` case is broken too, silently.

## Root cause (2026-09-13) -- one spec, `Hash[int]` baked in

Not a disagreement between two hash functions.  `set-add-elem__` is emitted as
**one** carrier-typed body with the `int` instances hard-wired, and every
element type is routed through it:

```c
static int64_t set_hyadd_hyelem_un_un(int64_t s, int64_t x) {
        int64_t __ps_233 = (__inst_Hash_hash_int(x));
        int64_t __ps_234 = (__inst_MapKey_mk_hybox_int(x));
        int64_t __ps_235 = (__inst_MapKey_mk_hycmp_int(x));
        int64_t __ps_236 = (__inst_MapKey_mk_hyowned_qu_int(x));
        ...
}
```

`__inst_Hash_hash_int` is the identity, so a `Sym` element is keyed by its
interned POINTER while `set-member?`'s `(hash (quote x))` correctly resolves
`__inst_Hash_hash_Sym` and reads the symbol's precomputed hash field.  The two
never agree.  `set-count` is right because the insert did happen -- under the
wrong key.

**`mk-cmp` is baked the same way, which the filing did not catch.**  A
`(Set cstr)` built through `set-add-elem__` gets `MapKey[int]`'s comparator, so
it is keyed by ADDRESS, not content -- two equal strings at distinct addresses
are two members.  A probe with two `"hi"` literals looks correct only because
the compiler pools them at one address.  That is a second silent wrong answer
from the same cause, and it is the one the report's "which one is wrong is the
open question" framing would have missed.

## The trigger is the RECEIVER, not the element -- measured

| call | result |
| --- | --- |
| `(set-add-elem__ (set-new) (quote x))` | **BROKEN** -- carrier spec |
| `(let [s : (Set Sym) (set-new)] (set-add-elem__ s (quote x)))` | ok -- real monomorph |
| `(set-of "aa" "bb")` then `set-member?` | ok |

So the workaround is to **annotate the receiver**.  With `s : (Set Sym)` in
scope the call mints
`set_add_elem____spec__tur_adt_Set__sym___tur_adt_Set__sym___const_struct___tur_sym__`
and resolves `Hash[Sym]` / `MapKey[Sym]` properly.  `spices/crdt`'s choice of
the explicit-hash `set-add` / `set-member?` pair is still a fine one, but it is
not the only way out.

## Why `A` never grounds -- `call_collect_type_bindings`, `elab_call.c:952`

Bindings are collected left to right over the arguments.

1. Arg 1 is `(set-new)`, whose type is `(Set A)` with `A` free.  Matching it
   against the parameter's `(Set A)` binds `A` to a TYVAR.
2. Arg 2 is the `Sym`.  The `TY_TYVAR` arm finds `A` already bound, sees the
   existing binding is a tyvar of the same name and the actual is concrete, and
   **accepts without overwriting**:

   ```c
   if (bindings[idx].type.kind == TY_TYVAR &&
       bindings[idx].type.as.tyvar_.name == expected->as.tyvar_.name &&
       actual.kind != TY_TYVAR) {
       return true;
   }
   ```

That is deliberate and load-bearing, with its own recorded history
(`m5-eq-vec-rewrite-fn-arg-loses-annotation` step 2, fix-i v2): the binding is
kept TYVAR so `emit_abi_type_has_concrete_named_tyvar` still sees the abstract
signal and routes through the relay path, and the comment records that a
"skip-and-upgrade" attempt regressed `hamt-delete`.  So the one-line
"overwrite the tyvar binding with the concrete one" fix is exactly the change
that has already been tried and reverted.  **Do not retry it without reading
that history first.**

## Reordering the parameters does not work either -- measured

Putting the concrete-typed parameter first so `A` grounds before the receiver
is seen turns the silent wrong answer into a hard error:

```turmeric
(defn my-add-elem [^Hash A ^MapKey A] [x : A s : (Set A)] : (Set A) ...)
(my-add-elem (quote x) (set-new))
```

```
error [TUR-E0001]: function 'my-add-elem' arg 2:
  expected (type-app Set tyvar 'A'), got (type-app Set tyvar 'A')
```

`A` is now bound to `Sym`, and the fresh `(set-new)`'s own open `(Set A)` does
not unify FORWARD against it -- the empty container soundly inhabits `(Set Sym)`
but nothing says so.  There is already machinery for precisely this, W2 ("fresh
empty-container forward unification", `elab_call.c` ~6488), but it fires only
when the parameter type is literally concrete, not when its tyvars are concrete
only via `type_bindings`.

Note the diagnostic prints the two sides identically, which makes this look like
a compiler bug report rather than a unification failure.  Worth fixing on its own
(print the binding, or disambiguate the tyvars) whoever picks this up.

## Fix direction (revised)

Two independent halves, either of which closes the repro:

1. **Substitute before comparing.** Before checking an argument against a
   parameter type, substitute the tyvars already bound in `type_bindings`, and
   let W2's forward unification accept a free empty-container argument against
   the substituted (now concrete) parameter.  This fixes the reordered spelling
   and any call where a concrete argument precedes the open container.
2. **A second pass.** Collect bindings over all arguments once, then re-run the
   ones that bound only a tyvar.  This fixes the shape as written, without
   touching the guarded no-overwrite rule: nothing is overwritten during the
   first pass, and the second pass starts from a set in which `A` is concrete.

(1) is smaller; (2) is the one that fixes the API as it is spelled today.

## Not a stdlib fix

Routing `set-add1` through a call-site macro (the way `set-add` already
dispatches `MapKey` on the concrete element EXPRESSION) would sidestep it, but it
gives up the typed homogeneity check that `set-add-elem__` exists to provide --
`set-of-element-type-is-not-checked`, a filed and fixed bug -- and would change
the diagnostic its error fixture pins.  The defect is in inference; that is where
it should be fixed.

## Repro

```turmeric
(defn main [] : int
  (let [a  (set-add-elem__ (set-new) (quote x))
        a2 (set-add-elem__ a (quote y))]
    (println (set-count a2))                                ;; 2  -- both added
    (println (set-member? a2 (hash (quote x)) (quote x))))  ;; false -- WRONG
  0)
```

The same set built through the `set-add` macro is fine:

```turmeric
(let [s (set-add (set-new) (hash (quote x)) (quote x))]
  (println (set-count s))                              ;; 1
  (println (set-member? s (hash (quote x)) (quote x))))  ;; true
```

## Mechanism

`set-add` and `set-member?` (`stdlib/set.tur:127`, `:173`) are macros taking an
explicit hash and forwarding it to `set-add-eq-o` / `set-has-eq-o?` alongside
`mk-box` / `mk-cmp` / `mk-owned?`. Given the same `h` on both sides they agree,
which is what the second snippet shows.

`set-add-elem__` (`:524`) is the typed `[^Hash A ^MapKey A]` entry point and
computes the hash itself. Whatever it computes is not what `(hash x)` returns
for the same element, so a set built with it cannot be queried with the macro.

Which one is wrong is the open question. If `set-add-elem__`'s internal hash is
the intended one, then `set-member?` needs a typed twin that computes the hash
the same way -- there is `set-add1` (`:545`, sugar for `set-add-elem__`) but no
`set-member1`, which is probably how the gap survived.

## Why it bit here

A G-Set's entire content is "add elements, union two sets, ask whether an
element is present". The typed adder is the one a reader reaches for first --
it needs no hash argument and has the `Hash`/`MapKey` constraints that suggest
it is the principled entry point.

`spices/crdt` uses the explicit-hash `set-add` / `set-member?` pair throughout
as a result, with a comment pointing here.

## Fixture owed

The repro above, asserting `true`. Note that `set-count` alone passes either
way -- the assertion has to be a membership test, which is why a fixture that
only checked sizes would not have caught this.

Add a `cstr` sibling too, per the root cause above: two equal strings built at
DISTINCT addresses (e.g. via `str-concat`) inserted through `set-add-elem__`
must collapse to one member.  A fixture using two `"hi"` literals passes even
broken, because the compiler pools them.
