---
title: Under `--interpret`, a value built by a native ADT constructor reports its CONSTRUCTOR name from `type-of`, not its ADT name
category: Archive
description: `(defn f [] : any (some 7.1))` gives type-of "Option" compiled and "Some" interpreted. `some` is a native override (interpreter_natives.c), and the TuriStruct it builds has no ctor->adt link, so turi_any_named_type falls through to v.as_struct->name -- the constructor. The same program written with the ADT constructor directly, or through a user-defined generic wrapper, agrees on both paths.
---

# A native ADT constructor loses the ADT name under `--interpret`

**RESOLVED 2026-09-07** via fix direction 1 -- attach the link where the value
is built -- after running the sweep the report asked for first.

**The severity was understated, and the sweep is what showed it.** The report
framed this as "not a wrong *answer* so much as a different one". It is a wrong
answer: `is?` compares exactly the names `type-of` reports, so
`(is? x (Option float))` on a natively-built option was a **false negative**
under `--interpret` -- 1 compiled, 0 interpreted -- and a type-case written
against the compiled behaviour took the wrong arm with no diagnostic. That is
the assertion the fixture leads with.

**Scope, measured rather than assumed.** Every native that builds an ADT value
goes through one function, `turi_make_struct`, and it has exactly eleven call
sites: `Some`/`None` (from `some`, `none`, and `option-map`), `Ok`/`Err`, the
`Left`/`Right` pair `str-to-int-checked` builds, and one FFI record named
`"Result"` that is not an ADT constructor at all. So the whole population is
reachable from a single choke point, and `some` was not special.

**The fix.** There is no ADT registry on the interpreter's env -- the only
runtime handle on a `CtorDef` is the binding `EX_DEFDATA` registers for each
constructor, a native closure over `adt_ctor_native` whose user data *is* the
`CtorDef`. `turi_make_struct` now looks the constructor name up in the env and
matches on that function pointer, which is what makes it safe: an ordinary
`defn` or another native sharing a constructor's name does not match, and a name
with no such binding (the `"Result"` FFI record) leaves the ctor NULL exactly as
before. It uses `turi_env_find_binding`, not `turi_env_get`, because a miss from
the latter formats an "unbound variable" string -- a malloc on every
construction that is not an ADT ctor.

Attaching the link at construction rather than resolving it inside
`turi_any_named_type` (direction 2) fixes every reader at once: field access by
name and the `is_heap` copy rule read `ctor` too, and it leaves that function's
fallback meaning what it is for -- a value that genuinely has no constructor.

**What is NOT this defect, and was checked.** With the names agreeing,
`(is? (none) (Option float))` and the two `Result` shapes still answer 0
compiled / 1 interpreted. That is the per-instantiation vs head-match residual
documented in
[any-narrowing-broken-for-parametric-receivers](any-narrowing-broken-for-parametric-receivers.md),
and a ctor-built `(Some 7.1)` shows the identical pattern against
`(Option int)` -- so it is independent of the native path. The fixture asserts
only shapes that agree, and says why.

**One neighbouring defect the sweep turned up**, filed separately because the
root cause is different -- there is no struct at all, rather than a struct
missing a link:
[interp-collection-handles-report-as-int](../reported/interp-collection-handles-report-as-int.md).
A Vec or Map is a raw `TURI_INT` carrier in the interpreter, so `type-of`
answers "int" and `is?` is wrong in *both* directions -- a false negative on
`(Vec int)` and a false positive on `int`.

Pinned by `tests/fixtures/interp-native-ctor-adt-name`, which both suites run.

---

**Severity: medium.** A silent compiled/interpreted divergence in the `any`
reflection surface. It is not a wrong *answer* so much as a different one, but
`type-of` is a string a program can branch on, so a type-case written against
the compiled behaviour takes the wrong arm under the interpreter.

Found while fixing
[generic-fn-in-any-return-position-emits-uncompilable-c](generic-fn-in-any-return-position-emits-uncompilable-c.md):
that fix made the compiled side of `(defn f [] : any (some 7.1))` start
working, which is what exposed this. The interpreter's behaviour is
pre-existing -- before the fix the program did not compile at all, so there was
no compiled answer to disagree with.

## Repro (2026-09-07, after the P2d fix)

```turmeric
(defn f [] : any (some 7.1))
(defn main [] : int (println (type-of (f))) 0)
```

```
$ tur run p.tur                                  => Option
$ ASAN_OPTIONS=detect_leaks=0 tur --interpret p.tur   => Some
```

Two neighbouring shapes agree on both paths, which is what localises it:

| program | compiled | interpreted |
| --- | --- | --- |
| `(defn f [] : any (Some 7.1))` -- ADT ctor directly | `Option` | `Option` |
| `(defn mk [A] [v : A] : (Option A) (Some v))` then `(mk 7.1)` | `Option` | `Option` |
| `(defn f [] : any (some 7.1))` -- the stdlib wrapper | `Option` | **`Some`** |

## Root cause

`some` is not interpreted as the stdlib Turmeric definition; it is a **native
override**:

```c
/* src/turi/interpreter_natives.c:3048 */
turi_env_register_native(env, "some", native_some, NULL);
```

`turi_any_named_type` (`src/turi/eval.c`) prefers the ADT name and falls back
to the struct's own name:

```c
if (!turi_struct_is_struct_like(v) && v.as_struct->ctor &&
    v.as_struct->ctor->adt && v.as_struct->ctor->adt->name)
    return v.as_struct->ctor->adt->name;
return v.as_struct->name;
```

The value `native_some` builds has no `ctor->adt` link, so the fallback fires
and returns the constructor name. A value built by evaluating the ordinary
`(Some x)` form does carry the link, which is why the first two rows agree.

## Scope -- not established, and probably wider than `some`

The probe covers `some`. Every native that constructs an ADT value is a
candidate for the same gap, and `interpreter_natives.c` registers many. **Sweep
before fixing**: the cheap check is a fixture that round-trips `type-of` for
each natively-constructed stdlib value (`some`/`none`, `ok`/`err`, the Cons
list constructors, the collection builders) and compares the two back ends.
That sweep is worth doing regardless of the fix, because nothing currently
compares `type-of` across back ends for any of them.

## Fix directions

1. **Attach the link where the value is built.** Whatever helper the natives
   use to construct an ADT value should set `ctor->adt` the way the evaluated
   `(Some x)` path does. This is the real fix and makes the fallback in
   `turi_any_named_type` unreachable for ADT values, which is the right state:
   the fallback exists for struct-likes.
2. **Failing that, resolve the name at lookup.** `turi_any_named_type` could
   map a constructor name to its ADT via the interpreter's ADT registry before
   falling back. Cheaper, but it papers over a value that is genuinely missing
   a link other code may also want.

Whichever lands, the fixture should assert `type-of` **on both back ends** for
the same program -- the divergence survived precisely because nothing did.
