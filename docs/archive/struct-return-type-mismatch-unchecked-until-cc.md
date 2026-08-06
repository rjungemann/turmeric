# A return type that disagrees with the body is unchecked whenever a struct/ADT is involved

**Severity:** medium. Not a wrong answer -- the program never runs -- but the
failure is a `cc` error inside generated C, with no source span, no diagnostic
code, and a message about `tur_adt_*` types the user never wrote. Primitive
mismatches of exactly the same shape are caught properly, so the hole is
invisible until you happen to put a struct on one side.

## Summary

`tur check` accepts a function whose declared return type does not match the
type of its body, as long as a struct/ADT is on one side of the mismatch. The
error surfaces only when the emitted C is compiled.

```turmeric
(defstruct S [a : int])
(defn f [x : S] : int x)     ; returns S, declared int
(defn main [] : int 0)
```

```
$ tur check t.tur
$ echo $?
0
$ tur run t.tur
/tmp/tur-build/t.c:7171:16: error: incompatible types when returning type
  'tur_adt_S' but 'int64_t' {aka 'long int'} was expected
```

## Which shapes are checked

Probed with the same two-line program, varying only the parameter and return
types:

| Body type -> declared return | Result |
| --- | --- |
| `S` -> `:int` | **check OK, cc error** |
| `S` -> `:cstr` | **check OK, cc error** |
| `int` -> `:S` | **check OK, cc error** |
| `S` -> `:float` | check rejects |
| `float` -> `:int` | check rejects |
| `cstr` -> `:int` | check rejects |

So the checker does compare declared-vs-actual return types; it is the
struct/ADT arm of that comparison that is missing. `float` is the odd one out
among the struct cases and is presumably caught by a separate float-carrier
check rather than by the return-type comparison.

## Fix direction

Find where the return-type comparison runs and extend it to `TY_ADT` on either
side, or find what makes an ADT compare equal to `TY_INT` there and stop it. A
plausible cause is the carrier representation -- structs are int64-carried in
some paths -- being consulted for a check that should be on the surface type;
if so, this is a `docs/guides/value-representations-guide.md` open-cell in
disguise and should get a row there too. The diagnostic should be an ordinary
located type error naming both types.

## Found while

Executing
[fixture-dirs-with-loose-tur-files-pass-without-running](../archive/fixture-dirs-with-loose-tur-files-pass-without-running.md).
`tests/fixtures/typeclass/parametric-clone-list.tur` was one of 30 `.tur` files
in four directories that the suite reported PASS without running. Its instance
body was `(clone [x] x)` against a method declared `: int`, which is this
defect with a typeclass wrapped around it -- the plain `defn` above reproduces
it with no typeclass at all. The fixture has been rewritten to test what its
name says; this defect is what it was accidentally sitting on.

---

## Execution -- RESOLVED 2026-08-06

Fixed in the existing return-position dispatcher. Every row of the table above
reproduced as filed.

### The hole was deliberate, and the fix is one more predicate

The report guesses the checker is "missing its struct/ADT arm". It is not
missing -- it is *tolerated*, on purpose, and the code says so:

> A non-nominal body (primitive, opaque-int carrier, tyvar, applied type,
> unknown/inline-C) is tolerated: those are exactly the int64 carrier /
> by-value bridges the ABI relies on, not a soundly-rejectable mismatch.

That reasoning is right for the cases it was written for and wrong for this one.
Every tolerance in `return_position_conflict` exists because **both sides are
`int64_t` in the emitted C** -- a bare integer where a handle is declared really
is the same machine value. A by-value record ADT is not: it lowers to a real
`tur_adt_S` aggregate, so there is no shared representation and nothing to
bridge. So this is not a missing arm but a missing *distinction*, and it slots
into the existing structure as one more predicate
(`return_type_carrier_aggregate_conflict`) and one more `RET_CONFLICT_*` code.

**Membership is decided by asking `type_c_name`** -- the same function codegen
uses to spell the C type -- rather than by re-enumerating which ADTs are
by-value. A transparent int newtype (`defopaque H :int`) answers `int64_t` and
stays tolerated without the predicate having to know it exists.

### Three things the report did not have

**1. It is not gated on the return class, and that is the point.** The two
neighbouring checks (reverse pointer-scalar, bool-vs-integer) fire only for
`RET_CLASS_COMMITTED`, because a generic or `#{Unsafe}` function legitimately
uses the carrier spelling. Gating this one the same way looked right and left
the reported case unfixed: a **typeclass instance method** is
`RET_CLASS_CARRIER_METHOD`, and the shape that started this whole thread --
`tests/fixtures/typeclass/parametric-clone-list.tur`, an instance method
declared `: int` whose body returns a record ADT -- still reached `cc`. No
carrier class can make a `tur_adt_S` interchangeable with an `int64_t`, so the
aggregate check runs for all three. The full suite confirms nothing legitimate
depended on the tolerance.

**2. The interpreted path must be exempt, and a fixture proved it.** By-value
aggregates are a property of the compiled path; the tree-walking interpreter
boxes every value as a handle, so there an ADT under an integer return *is* a
genuine bridge. Programs write exactly that deliberately: the `:turi` arm of a
`#?(:tur ... :turi ...)` reader conditional whose `:tur` arm is inline-C
returning a boxed pointer. `map-multiword-struct-key` and
`set-multiword-struct-element` both do it and both went red until the predicate
short-circuited on `g_interpret_mode`.

**3. A `:heap` ADT application is the same defect one warning away.**
`(defn f [x : (Cons int)] : int x)` was not in the report's table and appeared
to be a working carrier bridge. It compiles -- but only because the emitted C's
*"returning 'long int' from a function with return type 'tur_adt_Cons__int *'
makes pointer from integer without a cast"* is a `-Wint-conversion` **warning**
rather than an error. Under `-Werror` it is the identical hard failure. A
`:heap` ADT-app lowers to a typed pointer, which is no more the carrier than a
struct is, so it is rejected too. A **non-heap** parametric app (`(Option int)`)
is left alone: its return crossing genuinely grounds it and it emits clean. That
asymmetry is why the predicate does not simply ask `type_c_name` for `TY_APP` --
doing so rejects four working shapes.

### Fixtures

Four `errors/` negatives, each `requires.compiled` since the interpreted path
accepts all of them by design, plus one positive pinning the bridges that must
survive:

| Fixture | Pins |
| --- | --- |
| `errors/return-type-aggregate-vs-scalar` | record ADT body, `: int` declared |
| `errors/return-type-scalar-vs-aggregate` | the reverse direction |
| `errors/return-type-heap-app-vs-scalar` | the `-Wint-conversion` corner (`(Cons int)` under `: int`) |
| `errors/instance-method-return-aggregate-vs-scalar` | the class-gate case; the `parametric-clone-list` shape |
| `return-type-carrier-bridges-still-accepted` | `defopaque` -> int, `(Option int)` -> int, a generic defn, and `S` -> `S`, all still compiling and printing |

`TUR-E0709`'s `tur explain` text gains the new case, including the note that
it -- unlike cases 1 and 2 -- has no `#{Unsafe}`/type-parameter escape hatch.

Suites: `bash tests/run.sh` 2589 passed, 0 failed; `bash tests/run-turi.sh`
1776 passed, 0 failed, 705 skipped. No snapshot churn.

### What this does not change

The report speculates this might be "a `value-representations-guide.md`
open-cell in disguise". It is not -- no representation changed, and no crossing
was added or removed. What changed is that the return position now asks whether
the two sides *share* a representation before tolerating a mismatch between
them. The `(Cons int)` finding above is the one thread that does touch the
repr campaign: that shape was relying on a C warning, and the fact that it had
to be found by reading `cc` output rather than by any check suggested other
`-Wint-conversion` sites might be worth a sweep.

**That sweep has since been run: 0 hits across 2563 fixtures**, built the way
the suite builds them. The corpus is clean, and what remains is that nothing
keeps it that way -- `cc` warnings are discarded on a successful build. Filed as
[emitted-c-pointer-integer-warnings-unwatched](../reported/emitted-c-pointer-integer-warnings-unwatched.md),
which also records the two methodology traps the first passes hit.
