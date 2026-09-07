---
title: An unannotated `defn` return is TY_NIL, not inferred -- so a float body errors, naming a `: nil` the programmer never wrote
category: Archive
description: RESOLVED 2026-09-07. Corrected diagnosis: elab_defn DOES infer the body's type (elab_fns.c ~8442) -- that block just runs AFTER the return-position conflict check, so the check compared the body against the un-inferred TY_NIL default. The fix skips the check when the return is unannotated, rather than porting an inference step that already existed. Original text: elab_defn starts return_kind at TY_NIL and never adopts the body's type when the return was not annotated (elab_fn does, at elab_fns.c:10036). An int or cstr body bridges through the int64 carrier and appears to work; a float body hits the register-class check and reports "declares return type 'nil'" for a defn with no return annotation at all.
---

# An unannotated `defn` return is `TY_NIL`, not inferred

**RESOLVED 2026-09-07 -- and the filed diagnosis was wrong in a way worth
recording.**

The report said `elab_defn` "never adopts the body's type" and proposed porting
`elab_fn`'s inference step across. It does adopt it: the block is right there
(`elab_fns.c`, "Infer return type from body if not specified or polymorphic"),
which is why `int64_t n()` and `const char *greet()` were already the emitted
signatures. **The bug was pure ordering** -- that block runs after the scope
pop, while the return-position conflict check runs well before it, so the check
compared the body's type against a `TY_NIL` default the inference had not
replaced yet.

So the fix is not fix direction 1. **The conflict check is skipped when the
return is unannotated**, because an unannotated return has nothing to conflict
*with*: the inference below will adopt whatever the body produced. Concretely
`(!return_annotated && return_kind == TY_NIL)`.

Both halves of that conjunct earn their place. `return_annotated` is what
separates the pair the surrounding code already flags as indistinguishable by
kind -- unannotated versus a written `: nil`/`: void`, which both leave
`return_kind == TY_NIL` and of which only the second is a real declaration.
And `return_kind == TY_NIL` keeps it narrow: the annotation paths that reach
`done_return_annotation` by `goto` set a real kind without setting
`return_annotated`, and those must still be checked.

This also delivers fix direction 3 by making it unnecessary -- the diagnostic
can no longer name a `nil` the programmer did not write, because it no longer
fires in that case at all.

**Verified across the matrix**, since the risk here is silently dropping a
check rather than fixing one:

| case | before | after |
| --- | --- | --- |
| unannotated, `42` / `"hi"` / `true` body | worked (carrier bridge) | works |
| unannotated, `7.1` body | **TUR-E0707** | **works** -- emits `static double pi()`, and a caller chaining `(* (pi) 2.0)` gets 14.2 |
| `: nil` / `: void`, float body | error | **still errors** |
| `: int`, float body | error | still errors |
| `: float`, cstr body | error | still errors |
| `: cstr`, int body | error | still errors |
| `: float`, float body | worked | works |

`7.1` rather than `7.0` throughout: a float return lives in a different
register class from the carrier, so an integral literal cannot distinguish a
real `double` signature from a bridge that happens to survive.

**Fixtures:** `defn-unannotated-return-inferred` (all four body types, plus a
call-site chain so the inferred type has to be right for callers too) and
`errors/return-type-declared-nil-float-body`, which pins the exact
discrimination the fix turns on -- if `return_annotated` ever stops being set,
that fixture goes red instead of the diagnostic vanishing silently. The
existing `errors/return-type-register-class-*` and
`errors/return-type-pointer-scalar-*` fixtures cover the annotated mismatches
and were unaffected.

**Suites at the fix:** `run.sh` 2837/0, `run-turi.sh` 1931/0,
`check-examples.sh` 38/0, `run-stdlib-checks.sh` 35/0. No snapshot churn --
elaboration, not codegen.

---

## The original report

**Severity: medium.** Three unannotated `defn` returns, three different
behaviours, and the failing one blames a declaration the programmer did not
write. Filed rather than fixed because the fix is a return-inference decision
(adopt the body's type, or require the annotation) that belongs with the
`saffron` dialect work, which flips this exact default --
[docs/upcoming/saffron-lang-plan.md](../upcoming/saffron-lang-plan.md) S2.

## Repro (v0.44.2, `2da89e84`)

```turmeric
(defn n [] 42)                   ;; => prints 42     (works)
(defn greet [] "hello")          ;; => prints hello  (works)
(defn pi [] 7.1)                 ;; => TUR-E0707
```

```
$ printf '(defn pi [] 7.1)\n(defn main [] : int (println (pi)) 0)\n' > p.tur
$ tur run p.tur
p.tur:1:13: error [TUR-E0707]: function 'pi' declares return type 'nil' but its
  body returns float -- a float and a non-float live in different register
  classes (xmm vs general-purpose), so this is a register-class miscompile, not
  a tolerable carrier bridge
1 | (defn pi [] 7.1)
  |             ^^^
```

`pi` declares nothing. The message names `nil` because that is the *default*,
not a declaration.

Reproduces identically under `--interpret` (shared elaborator).

## Root cause

`elab_defn` initialises the return kind to `TY_NIL` and never revises it when
the return was unannotated:

- `src/compiler/elab_fns.c:6811` -- `TypeKind return_kind = TY_NIL;`
- `src/compiler/elab_fns.c:6818` -- `bool return_annotated = false;`, set at
  `:7373`.
- `return_annotated` is then consulted at exactly **one** site,
  `:8130` (`check_nil_body`). There is no `if (!return_annotated) return_kind =
  body->type.kind;`.

The `fn` path has that step and `defn` does not:

```c
/* src/compiler/elab_fns.c:10036 -- elab_fn */
if (!return_annotated && return_kind == TY_NIL && body->type.kind != TY_NIL) {
```

So for an unannotated `defn` the declared return stays `TY_NIL` into
`return_position_conflict` (`:8132`), and the outcome depends on whether the
body's type can ride the int64 carrier:

| body | carrier-compatible with `nil`? | result |
|---|---|---|
| `42` (int) | yes | no conflict; call sites read it as int |
| `"hello"` (cstr) | yes (pointer in the carrier word) | no conflict |
| `7.1` (float) | **no** -- different register class | `RET_CONFLICT_REGISTER_CLASS` -> TUR-E0707 |

The register-class check is right to fire; a float return really cannot bridge.
The defect is that it fires against a *default* that was never a commitment,
and reports it as one.

The code already knows this is a trap. `:8125`:

> `return_kind` starts `TY_NIL`, so unannotated and `: void` are
> indistinguishable by kind

That comment is about `check_nil_body`; the same indistinguishability leaks
into the diagnostic text one branch over.

## Fix directions

1. **Infer, like `elab_fn` does.** Port `:10036` into `elab_defn` before the
   conflict dispatcher. Makes all three cases work and is the smallest change.
   Risk: an unannotated `defn` whose body type is not what the author expected
   silently gets that type instead of erroring -- the same trade `fn` already
   makes.
2. **Track "unannotated" separately from `TY_NIL`** and, when the return was
   never written, phrase the diagnostic as *"function 'pi' has no return
   annotation and its body returns float; annotate it `: float`"*. Keeps the
   error, fixes the lie. Smallest honest change if (1) is judged too loose.
3. Either way, `return_annotated` should reach the diagnostic at `:8138`, which
   currently prints `typekind_to_string(return_kind)` unconditionally.

Whichever lands, `TY_ANY` is already exempt from the nil-body conflict
(`elab_core.c:2480`), so a future `#lang saffron` default of `any` does not
re-enter this path.
