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
