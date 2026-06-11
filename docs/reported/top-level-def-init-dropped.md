---
title: Top-level `(def name init)` declares the binding but never emits the initializer
category: Reported
severity: Silent miscompile -- every top-level def reads as 0 at runtime
discovered: 2026-06-11, while writing tests for ECS spice E2 stage scheduler
resolved: 2026-06-11. Fix in `src/compiler/emit_module.c` -- adds a dedicated
  `def_init_body` buffer that EX_DEF / EX_DEFDYNAMIC initializers write into,
  and wires it into a `__attribute__((constructor))`-tagged
  `__tur_module_def_init` function when `user_has_main` is true so the runtime
  invokes it before main(). When the user has no main, def inits run first
  in the synthesized main, before any other top-level statements.
location: `src/compiler/emit_module.c` -- def-initialization emit path
---

# Top-level `(def name init)` declares the binding but never emits the initializer

> **Status: fixed 2026-06-11.** The minimal repro `(def y 7) ... (println y)`
> now prints `7`. `stdlib/math.tur::PI` reads as `3.14159...` from any caller.
> The ECS spice's `stage-pair.tur` uses real `(def pos-cid 0) (def hp-cid 2)`
> declarations and the conflict-check predicate reports `conflict-check-ok`
> as expected. Regression covered by
> `tests/fixtures/top-level-def-init-runs-before-main/`.
>
> The fix splits the file-scope emit's `body` buffer into two: `body` (top-
> level statements other than def inits) and `def_init_body` (just the def
> inits). Under `user_has_main`, only `def_init_body` flows into the
> constructor; `body` is dropped as before, preserving the long-standing
> "top-level non-def statements after a user main are silently dropped"
> behaviour. The single-file emit path is fixed; the separate-compilation
> emit path at line 6520 has the same bug but is not fixed in this pass
> (most fixtures use single-file mode; flagged as a follow-up).

## Summary

A top-level `(def name expr)` form -- in a file, inside a `defmodule`,
or in `stdlib/*.tur` -- elaborates without diagnostic, emits a
`static <T> <name>;` declaration in the generated C, and **never
emits the `<name> = <expr>;` initializer**. Every access of the def
at runtime reads zero-initialized bytes from the static-storage
default.

The bug is silent. The Turmeric-level `(println y)` of a `(def y 7)`
prints `0` with no warning. Inline-C bodies that reference the
def's mangled identifier fail at C compile time with "undeclared
identifier" -- a hint that the C-level binding isn't even
participating in the linker pass, suggesting the init emission is
dropped in a path that also skips header declaration in the cross-
TU case.

## Severity

Silent miscompile. `def` is used throughout the codebase for
constants:

- `stdlib/math.tur` -- `(def PI 3.141592653589793)`
- `stdlib/reactor.tur` -- `(def READ 1)`, `(def WRITE 2)`,
  `(def ERROR 4)`, `(def HUP 8)`
- `stdlib/schema.tur` -- `(def SCHEMA_STR 0)`, ...,
  `(def SCHEMA_NIL 4)`

If any of these compile to "static int64_t PI; ... (never set)"
and downstream code reads PI as `0.0`, math built on `PI` is
silently wrong. The schema-tag constants would all collapse to
`SCHEMA_STR` because they all read 0. Reactor event-mask checks
would all match `READ` for the same reason.

I have NOT verified whether stdlib's call sites of these constants
exhibit the wrong-value behaviour; they may be the original
trigger of every "this stdlib feature mostly works but the numbers
look off" bug we haven't tracked down. Anyone touching that area
should audit stdlib first.

## Minimal repro

```turmeric
(def y 7)

(defn main [] : int
  (println y)   ;; prints 0, not 7
  0)
```

Output: `0`.

Equivalent inline-C access fails earlier:

```turmeric
(def y 7)

(defn read-y [] : int
  ```c return y; ```)

(defn main [] : int (println (read-y)) 0)
```

Compile error: `error: use of undeclared identifier 'y'`.

The Turmeric-level access compiles because the elaborator resolves
the binding to its mangled global (e.g. `y_888`) which IS declared
in the generated C; but the inline-C body uses the source-level
name `y`, which has no header symbol because the cross-TU export
machinery also drops the binding when the init isn't emitted.

## Observed vs. expected

Observed: `(def y 7)` emits `static int64_t y_888;` at file scope
(verified in
`/tmp/tur-build/_tmp_test-def-bare_tur.c:4467` after `tur run`).
The initializer `y_888 = 7;` is never emitted -- searching the
generated C for `y_888 =` finds no match. Reads of `y` resolve to
`y_888` and read its zero-initialized bytes.

Expected: every `(def name init)` should emit both the declaration
*and* the initializer, with the initializer running at module load
time (or, equivalently, threaded through a module-init function
called from `main`).

## Root-cause pointer

`src/compiler/emit_module.c` around line 5360-5380 has the
EX_DEF handling that emits the static declaration:

```c
} else if (e->kind == EX_DEF) {
    /* Phase 11: skip struct typedefs — already emitted in Pass 0 */
    if (e->as.def_.struct_def) continue;
    char *bn = name_for_binding(&ctx, e->as.def_.binding);
    buf_printf(&file, "static %s %s;\n",
               type_c_name(e->as.def_.binding->type), bn);
    if (e->as.def_.init) {
        char *iv = emit_value(&ctx, &body, e->as.def_.init);
        indent_buf(&body, ctx.indent);
        buf_printf(&body, "%s = %s;\n", bn, iv);
        free(iv);
    }
    free(bn);
}
```

The initializer is emitted into a `body` buffer. The question is
whether that buffer is **wired into module initialization** -- i.e.
whether the generated code calls the init block before any defn
that references the def. If `body` is the body of a function that
is never called, or if the init block is emitted into a code path
that's pruned, the def is declared but never set.

The first thing to verify in a debugger: locate where the `body`
buffer is *consumed* and check whether the resulting function is
invoked before `main()` runs. A `__attribute__((constructor))`
wrapper around the init block is the standard fix if it's not
already there.

## Why this hasn't surfaced before

Best guess: most stdlib callers of `def`-constants happen to also
work if the value is 0:

- `(def SCHEMA_NIL 4)` -- if every schema-tag check defaults to
  `SCHEMA_STR == 0`, the schema typing all collapses to "everything
  is a string" -- the stdlib calls that use these tags may not have
  test coverage that distinguishes paths.
- `(def READ 1)` -- if every event-mask check returns `READ`, and
  the reactor flows `READ` paths, again no explicit failure.
- `(def PI 3.14...)` -- this WOULD fail loudly in any math that
  uses PI; if there's existing math code that calls PI without
  failing, either PI's actual value is recovered elsewhere or it
  isn't called in test runs.

The ECS spice tripped on it because two `def`-declared component
IDs (`POS-CID = 0`, `HP-CID = 2`) were used to compute disjoint
bitmasks; both reading as 0 made the masks collide and the
system-conflicts? predicate report a false positive.

## Workaround in the spice

`tests/stage-pair.tur` uses zero-arg defns instead:

```turmeric
(defn pos-cid [] : int 0)
(defn vel-cid [] : int 1)
(defn hp-cid  [] : int 2)
```

Verified end-to-end. The defn form goes through a different code
path that emits both declaration and definition correctly. This
adds one function-call worth of indirection per use; cheap, but
not what `def` is for.

## Proposed fix directions

1. **Wire the def-init `body` into a module-constructor.** Emit a
   `__attribute__((constructor))`-tagged init function whose body is
   the collected def initializers, and let the runtime call it
   before `main`. Standard C, supported by every relevant compiler.

2. **Emit defs with C-side initializers** when the init expression
   is a literal constant. `(def y 7)` -> `static int64_t y_888 = 7;`.
   Doesn't cover the general case (defs with non-constant init like
   `(def x (compute-something))`) but covers the common case in
   stdlib.

3. **Both.** Use C-side init for literal constants (faster, smaller
   image), use the constructor function for everything else. This
   matches what gcc/clang do for module-level data.

(1) alone is enough to close the bug. (2) is a perf optimization
that can land later.

## Validation plan

A fix is validated when:

- The minimal repro `(def y 7) ... (println y)` prints `7`.
- `(def READ 1)` from `stdlib/reactor.tur` reads as `1` (not `0`) at
  any call site, verified by a fixture that checks a reactor event
  mask.
- `(def PI 3.141592653589793)` from `stdlib/math.tur` reads as PI in
  a small fixture, not `0.0`.
- The stage-pair test in
  `../turmeric-spices/spices/ecs/tests/stage-pair.tur` can revert
  the defn-as-constant workaround and use `(def pos-cid 0)` etc.
  directly.

## Audit recommendation

After this lands, sweep every `def` site in `stdlib/` (and in the
sibling spices repo) and validate that callers were not silently
relying on the zero-fallback. Several may already be wrong in
production and have escaped notice because the failure mode is
"value is 0" rather than "crash."

## Interaction with the prereq plan

The ECS prerequisite plan
([`../upcoming/ecs-prereq-plan.md`](../upcoming/ecs-prereq-plan.md))
adds this as a sixth gap (F). It blocks no plan item directly --
the defn-as-constant workaround is fine -- but it makes constant
declarations meaningfully less ergonomic until fixed, and it's a
correctness bug whose blast radius extends beyond ECS into stdlib
itself.
