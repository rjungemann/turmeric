# C-name accessors that hand out a shared static buffer, to callers that collect before printing

**Severity: low as of today (no live wrong answer), but it is a repeat
offender** -- the same shape has produced two real miscompiles already, and the
failure mode is a silently wrong C type rather than a crash or a diagnostic.

**Status:** OPEN. Filed 2026-08-26 while fixing the second instance
([multi-variant-adts-always-heap-allocate](../archive/multi-variant-adts-always-heap-allocate.md),
SR1). One site remains, currently safe by accident of having exactly one caller.

## The shape

An accessor returns `const char *` naming a C type, and spells that name into a
**function-scoped `static` buffer**:

```c
static char ptrbuf[128];
snprintf(ptrbuf, sizeof(ptrbuf), "%s *", type_c_name(resolved));
return ptrbuf;
```

That is correct only while every caller consumes the result before asking for
another. Emitters routinely do the opposite: they gather one name per field or
per parameter into an array, and only then write the declaration. Every entry
then aliases the same buffer, so **every name is the LAST name**.

The reason it is worth a report rather than a note is what it looks like when it
fires. There is no crash, no ASan report and no compiler diagnostic -- the
emitted C is well-formed, it just has the wrong type in it. The bug surfaces
downstream as an `-Wincompatible-pointer-types` warning at some unrelated call
site, or not at all.

## Two instances, both already fixed

1. **`EmitSigEntry.ret_ctype`** (`emit_module.c`, the `g_sig_retired` block).
   Not a static buffer but the same lifetime error one step out:
   `emit_sig_lookup_ret_ctype` hands out the entry's interior pointer and
   callers hold it across further emission, so re-recording a return type freed
   it under them. Caught by ASan as a heap-use-after-free with 43 fixtures
   failing. Fixed by retiring strings instead of freeing them.

2. **`adt_field_c_type`** (`types.c`, the `adt_field_is_ros_pointer_box`
   branch). The monomorph constructor emitter fills `val_ctype[]` for every
   field and only then writes the parameter list, so a constructor with two
   pointer-boxed fields mistyped all but the last:

   ```c
   static tur_adt_Result__Rational__ArithError
   ctor_Result__Rational__ArithError(bool _0,
                                     tur_adt_ArithError *_1,   /* should be Rational * */
                                     tur_adt_ArithError *_2) { ... }
   ```

   Latent for as long as it took for a `Result`/`Option` monomorph to have
   non-parametric by-value ADTs in **both** arms, which is what a by-value sum
   makes ordinary. Fixed 2026-08-26 with a 16-slot rotating pool.

Note the second was found by a representation change, not by a test. Nothing in
the suite was looking for it.

## What is still live

**`ensure_static_fatbox`** (`emit_module.c`, two `static char name[96]` sites,
one per return path). It has exactly ONE caller (`emit_expr.c`, the bare-to-fat
static-box bridge) and that caller spells the name into a `buf_printf` on the
next line, so it is correct today. It is correct by call-site discipline, not by
construction: a second caller that holds two boxes -- or one that stashes the
name and emits later -- reintroduces the bug silently.

Two sites are **fine and should not be "fixed"**, recorded so a sweep does not
churn them:

- `kind_to_string`'s `static char fallback[64]` (`types.c`) -- arity >= 16 only,
  reached from diagnostics, and its comment already says "not reentrant; only
  for errors".
- `gs_iso_now` / `inst_iso_now` / `iso_now` (`global.c`, `install.c`, `pkg.c`)
  -- timestamp formatting, one value in flight, single-threaded by
  construction.

And one accessor is **safe by construction and worth knowing about**, because it
is the one most likely to be assumed unsafe: `type_c_name` (and its wrapper
`emit_type_c_name`) interns every composed name via `intern_type_name`, so the
pointers it returns are stable for the whole compilation and may be held freely.
The distinction between it and `adt_field_c_type` is invisible at the call site,
which is part of why instance 2 survived.

## Fix directions

In rough order of cost:

1. **`ensure_static_fatbox`**: give it the rotating pool, or have it return an
   arena/interned string. One function, no call-site change.
2. **Make the safe thing the default.** Route every C-name spelling through
   `intern_type_name` so the accessor's contract is uniformly "stable for the
   compilation". This is what `type_c_name` already does; the pointer-box
   spelling in `adt_field_c_type` is the odd one out, and would become a
   one-line `intern_type_name(b.data)` instead of a pool.
3. **A grep-level guard.** The shape is mechanically recognisable: a function
   returning `const char *` whose body writes a function-scoped `static char
   buf[...]`. A `tests/check-*.sh` ratchet over `src/compiler/` with the four
   known-benign sites listed would stop a third instance being written. This is
   the only one of the three that catches the NEXT one rather than the last.

## Repro (instance 2, before the fix)

```sh
git show 74b63b31~1 -- src/compiler/types.c   # the single-buffer version
# build, then:
TUR_SR1_SUM_BYVALUE=1 ./build/tur build tests/fixtures/rational-arith/input.tur -o /tmp/x
# cc: passing argument 2 of 'ctor_Result__Rational__ArithError'
#     from incompatible pointer type
```

## Guides to update when fixed

- [docs/guides/value-representations-guide.md](guides/value-representations-guide.md)
  -- the C-name accessors are part of the representation surface it documents,
  and the "which accessor's result may I hold?" question has no home there yet.
