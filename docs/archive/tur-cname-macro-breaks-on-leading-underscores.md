# `__TUR_CNAME_<name>__` does not expand when `<name>` starts with `__`

**Status:** RESOLVED 2026-08-18.
**Severity:** medium (silently emits a call to a nonexistent C function; the
resulting error names a mangled fragment that appears nowhere in the source)
**Found:** 2026-08-18, writing `spices/secret` against `tur` v0.35.0
(`2748f5e8a`).

## Summary

Inline-C bodies call sibling Turmeric defns through `__TUR_CNAME_<name>__`.
The trailing `__` is the terminator, so a `<name>` that itself begins with
`__` -- the ordinary convention for a private helper across this codebase and
the spices -- is not recognized and the macro is left unexpanded, or worse,
mis-split.

With a plain name the call fails loudly but comprehensibly:

```
error: call to undeclared function '__TUR_CNAME___helper__'
```

With a hyphenated name (`__secret-alloc`) the mangler produced a *different*
fragment entirely:

```
error: call to undeclared function 'alloc__'
```

`alloc__` appears nowhere in the `.tur` source, which makes this genuinely
hard to trace back to "your helper's name starts with two underscores."

## Minimal repro

```turmeric
(defmodule b3

(defn __helper [x : int] : int
  ```c
  return x + 1;
  ```)

(defn caller [x : int] : int
  ```c
  return __TUR_CNAME___helper__(x);
  ```)

(defn main [] : int (caller 1)))
```

```
$ tur run b3.tur
.../b3_tur.c:7183:16: error: call to undeclared function '__TUR_CNAME___helper__'
1 error generated.
```

Renaming `__helper` to `helper` (or `helper-raw`) fixes it.

## Expected

Either expand correctly for names with leading underscores, or -- if the
delimiter genuinely cannot be made unambiguous -- reject the reference with a
real diagnostic naming the helper and the rule, instead of emitting a call to
a function that was never defined.

Worth noting the convention collision: `__`-prefixed private helpers are used
throughout `stdlib/` (`__functor_*`, `tur-contract-check`) and the spices
(`__gzbuf`, `__db-prepare-raw`, `__cons`), so the name that trips this is the
one an author is most likely to pick for exactly the kind of helper that gets
called from inline C.

## Workaround

Name inline-C-callable helpers without a leading `__`.
`spices/secret/src/secret/core.tur` uses a `-raw` suffix instead
(`secret-alloc-raw`, `secret-free-raw!`, `secret-data-raw`), keeping them
unexported for privacy rather than relying on the name.

## RESOLVED (2026-08-18)

Root cause: the `__TUR_CNAME_<name>__` terminator scan starts at the name's
first character, so a name beginning with `__` matched the closing delimiter
at offset zero, the splice was left unexpanded, and the emitted C either
called a nonexistent `__TUR_CNAME_...` function or -- with a hyphen in the
name -- was tokenized as a subtraction (`__TUR_CNAME___secret - alloc__(n)`),
which is where the phantom `alloc__` came from.

The scan was duplicated at **three** sites: the emit-time mangle-only path
(`emit_core.c`) and both passes of the elaboration-time module-prefix
resolver (`elab_toplevel.c`). Fixing only the first is not enough -- it
mangles the name but drops the module prefix, so the call still does not
resolve. All three now share `tur_cname_name_len` in `mangle.h`, which skips
a leading underscore run when a real name character follows it.

An interior `__` still terminates the name; that ambiguity is inherent to the
delimiter and is unchanged.

Regression fixture: `tests/fixtures/inline-c-cname-leading-underscores/`,
covering all three shapes -- leading `__`, leading `__` plus a hyphen (the
mis-tokenizing variant), and a plain name as the control -- and asserting on
computed values rather than merely on compiling. Verified to FAIL against the
pre-fix compiler.

Full fixture suite: 2621 passed, 0 failed.
