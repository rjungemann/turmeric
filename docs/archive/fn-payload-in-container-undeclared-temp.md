# Calling a zero-arity float-returning fn read out of a container emits an undeclared temp

**Severity:** medium -- hard `cc` failure, not a miscompile, so it cannot ship
wrong answers. But it makes function values unusable as container payloads, and
the error names a generated identifier rather than anything in the source.

**Status:** RESOLVED 2026-07-31 by fn-value-fat-normalization stage 2 (docs/upcoming/fn-value-fat-normalization-plan.md): fn values stored in container payloads are uniform fat handles now; the defdata (float payload) and Vec variants both run correctly (a cosmetic -Wint-conversion warning remains at the ctor slot -- the int64-handle spelling family). Archived. Original status text follows.

**Status (original):** open, and its **failure mode changed on 2026-07-30**: it no longer
fails to compile, it compiles with a `-Wint-conversion` warning and
**segfaults**. See [Failure mode change](#failure-mode-change-2026-07-30).
Found 2026-07-29 while investigating
[concrete-codegen-layout-kind-enumerations-drift](../archive/concrete-codegen-layout-kind-enumerations-drift.md);
independent of it (see [Not the mangling collision](#not-the-mangling-collision)).

## Repro

Store a function value in a parametric ADT, pull it back out, call it:

```turmeric
(load "stdlib/math.tur")
(defdata Box [a] (MkBox a))

(defn mk-flt [] : (fn [] float) (fn [] 7.25))

(defn use-flt [b : (Box (fn [] float))] : float
  (match b
    (MkBox f) (f)))

(defn main [] : int
  (printf-float6 (use-flt (MkBox (mk-flt))))
  (println "")
  0)
```

```
$ tur run one.tur
/tmp/tur-build/one_tur.c:7080:25: error: '__ps_167' undeclared (first use in this function);
                                          did you mean '__ps_164'?
 7080 |         printf_hyfloat6(__ps_167);
      |                         ^~~~~~~~
tur: cc invocation failed (status 256)
```

The emitted C references `__ps_167` where only `__ps_164` was declared -- an
off-by-a-few in the generated-temp numbering, so a value the caller expects was
never materialised.

Reproduces the same way through `Vec` rather than a `defdata`:

```turmeric
(defn first-flt [v : (Vec (fn [] float))] : float
  (let [f (vec-get v 0)] (f)))
```

```
/tmp/tur-build/vecfn_tur.c:7064:54: error: '__ps_162' undeclared ...
```

So it is not specific to `defdata`, to `match`, or to the by-value product path
-- it follows *calling a function value that was read out of a container*. (That
`Vec` variant is also a `(fn [] float)`; see the narrowing below.)

## Not the mangling collision

This surfaced while trying to trigger the two-types-one-C-name collision in the
sibling report, and it is worth stating plainly that it is a different bug, since
the obvious reading is that the two same-named constructors are what broke the
build:

- **One instantiation is enough.** The repro above declares a single
  `(Box (fn [] float))`. A mangling collision needs two distinct types sharing a
  token; there is nothing to collide with here.
- **The same shape with non-function payloads compiles and runs.** `(Box float)`
  and `(Box int)` together, same `defdata` + `match` + extract shape:

  ```
  7.250000
  42.000000
  ```

  So neither payload extraction nor the parametric by-value product is at fault.

## Narrowed: zero arity AND a float result, both required

Measured, same `Box` + `match` + extract-and-call shape throughout, varying only
the boxed function's type:

| boxed fn type | result |
| --- | --- |
| `(fn [] float)` | **FAILS** -- `'__ps_167' undeclared` |
| `(fn [] int)` | compiles, runs |
| `(fn [int] int)` | compiles, runs |
| `(fn [int] float)` | compiles, runs -- prints `7.500000` |

So it is neither "boxed functions in containers" nor "float payloads" on their
own. It takes **arity zero together with a float result**. That combination puts
this in the same family as the tree's existing float-register-class defects
(`TUR-E0707`, and the `fn_type_has_float_carrier` special-casing in
`elab_call.c`): a niladic float thunk has no argument to force the call through
the normal temp-emitting path, and its result rides xmm rather than the int64
carrier.

CLAUDE.md's float rule is what made this visible at all -- the first probe used
`7.25`, and an integer literal would have shown nothing.


## Failure mode change (2026-07-30)

The fn-element spelling fix
([fn-element-tyvars-not-substituted-in-spec-types](../archive/fn-element-tyvars-not-substituted-in-spec-types.md))
changed how this shape fails, and **for the worse**:

```
before:  error: '__ps_167' undeclared            -- hard cc failure, no binary
after:   warning: passing argument 1 of 'ctor_MkBox__fn0__float' makes integer
                  from pointer without a cast [-Wint-conversion]
         Segmentation fault                       -- compiles, then crashes
```

The generated constructor is now `ctor_MkBox__fn0__float(int64_t _0)` and the
call passes a `tur_fnptr_double_t` (`double (*)(void)`), so the C compiler warns
and the program runs into a bad handle.

That fix is a net improvement overall -- it repaired two red fixtures and made
several monomorph names stop lying about their C types -- and no fixture covers
this shape, so the suite did not see it. But a compile error is a better resting
state than a segfault, and this report should be read as **higher priority than
its "medium" severity suggests** until that is true again.

The pairing is not accidental: this bug and that one are the same underlying
confusion about how a function value is represented when it is stored rather
than called. Whoever fixes this should expect the fix to be in that same area,
not in the temp emitter the original root-cause guess pointed at.

## Root cause direction (unproven)

The shape (`__ps_N` referenced but never declared) says the call-site lowering
advanced the temp counter on a path the statement emitter did not visit. Given
the narrowing above, the place to look is wherever a zero-argument application of
a boxed closure is lowered: with no arguments to emit, the float-result path
appears to allocate a result temp and then take a branch that never declares it.

## Why it matters beyond the immediate error

Narrowing this bug is what made the sibling report's collision reachable. While
the only probe was a `(fn [] float)` payload, that report concluded "no reachable
trigger"; once the table above showed `(fn [int] float)` and `(fn [int] int)` both
compile, two `Box` instantiations over them turned out to collide for real --
compiling, printing correct answers, and routing every closure handle through a
`double` that only round-trips below 2^53. So this bug was *masking* a live
silent-miscompile, not gating a hypothetical one.

Two consequences:

- The mangling fix is independently urgent; it does not depend on this one.
- Whoever fixes *this* one should re-check that report's reachability section
  rather than trusting it, since it has already been wrong once in this
  direction.
