# A call to a forward-referenced `: nil` function is bound to `__auto_type`

**Severity: medium** (clean build break, `cc`-level diagnostic with no `.tur`
attribution; trivial workaround once understood). Found 2026-08-28 getting
`turmeric-spices` CI green, against `tur v0.40.0` / turmeric `5c9d533`.

**Verified 2026-08-28** on a freshly built `v0.40.0` (macOS arm64 / Apple
clang), along with the six-variant narrowing below.

## Summary

A call in statement position to a `: nil` function that has **not yet been
elaborated** is emitted as `__auto_type __ps_N = <void call>;` instead of a bare
statement. Self-recursion is the commonest way to reach that state, but it is
not the trigger -- a plain forward reference does it with no recursion at all,
and `: void` never does it in either direction.

## Repro

```turmeric
(defmodule vr (export)
  (defn sink [x : int] : int ```c return x; ```)

  ;; `: nil` + self-recursion in the tail of a `when`
  (defn loop-nil [i : int n : int] : nil
    (when (< i n)
      (do (sink i)
          (loop-nil (+ i 1) n))))

  ;; identical shape, `: int` -- compiles fine
  (defn loop-int [i : int n : int] : int
    (if (< i n)
      (do (sink i)
          (loop-int (+ i 1) n))
      0))

  (defn main [] : int (do (loop-nil 0 3) (loop-int 0 3))))
```

`tur check` is silent. Emitted C:

```c
static void vr__loop_hynil(int64_t i, int64_t n) {
        if ((i) < (n)) {
            int64_t __ps_163 = (vr__sink(i));
            if (tur_panicking) return;
            (void)(__ps_163);
            __auto_type __ps_164 = (vr__loop_hynil((i) + (INT64_C(1)), n));   /* void */
            if (tur_panicking) return;
            (void)(__ps_164);
        } else {
        }
}
```

```
error: variable has incomplete type 'void'
```

Note `loop-int`, the same shape with an `: int` return, gets a proper
`__tur_tailcall:` loop and compiles.

## What actually narrows it -- it is forward reference, not recursion

The report this was filed from described it as "`: nil` + self-recursion". Six
variants say otherwise. Recursion is incidental; being *not yet defined* at the
call site is the trigger, and `: void` is immune.

| Variant | Result |
| --- | --- |
| `: nil` callee defined **after** the caller, **no recursion at all** | **error** |
| `: nil` callee defined **before** the caller | compiles |
| `: nil` self-recursive (one tail call, no `do`) | **error** |
| `: nil` mutual recursion (`g` -> `h` -> `g`) | **error** |
| `: void` self-recursive | compiles |
| `: void` callee defined **after** the caller | compiles |

The decisive pair is rows 1 and 2: the same two functions, the same call, only
their order in the file changes, and one compiles. The decisive pair for the
type is rows 1 and 6: same forward reference, `: void` instead of `: nil`, and
it is fine -- the emitter produces a bare `g__callee(i);`.

So the rule is: **a call in statement position to a `: nil` callee the emitter
has not yet seen.** Recursion (self or mutual) is just the way a call site is
guaranteed to precede its callee's definition.

That points the fix somewhere specific. The statement-position emitter asks the
callee for its C return type to decide between "bare statement" and "bind to a
temp". For an unresolved callee that lookup returns unknown and it falls back to
`__auto_type`. `: void` survives because it is resolved eagerly at that point
while `: nil` is not -- `nil` is evidently lowered to `void` later than the
lookup runs, so the two spellings of the same return type answer the question
differently.

## Expected

A call to a `void`-returning function in statement position should be emitted as
a bare statement, never bound to a temporary. `: nil` and `: void` should be
indistinguishable here.

## Relationship to the archived `let`-binds-void report

This is **not** a duplicate of
[`let-binding-void-call-emits-invalid-c`](../archive/let-binding-void-call-emits-invalid-c.md)
(RESOLVED 2026-08-18), though the `cc` error text is identical.

That report was about a *user-written* `let` binding a `:void` init, and its fix
was TUR-E0023 -- reject it in `elab_let`, tell the author to use `do`. Here the
binding is **synthesized by the emitter** for a call the author wrote in ordinary
statement position. There is no user binding to reject, and TUR-E0023 correctly
does not fire: the program is valid Turmeric and the workaround TUR-E0023
prescribes (`do`) is already what the source uses.

Worth reading them together anyway -- the earlier fix established that "the
rejected set is exactly the set that could not compile" for user bindings; the
same audit was evidently not extended to emitter-synthesized temps.

## Fix direction

Find the statement-position call emitter's return-type lookup and make it
resolve `nil` the way it resolves `void`. Two candidate shapes:

1. **Resolve `: nil` to `void` earlier**, so the callee's recorded return type
   is already `void` by the time any call site is emitted. This is the real fix
   if `nil`-to-`void` lowering is simply running too late, and it removes the
   whole class rather than this instance.
2. **Make the fallback safe.** Even with (1), `__auto_type` is the wrong default
   for an unresolved callee return type -- it cannot express `void` and there is
   no other case where guessing is better than a two-pass lookup. The emitter
   already forward-declares every function (`static void vr__loop_hynil(int64_t,
   int64_t);` is right there in the output), so the correct return type is
   available; the statement emitter just is not consulting the same table.

(2) is the smaller and more targeted change; (1) is worth checking for
regardless, because a `nil`/`void` divergence in one lookup suggests others.

A regression fixture should cover the **non-recursive forward-reference** row,
not just the self-recursive one -- a fix keyed on recursion would pass a
recursion-only fixture and leave rows 1 and 4 broken.

## Where it bit

`spices/ws-server` (`hub-send-each`) and `spices/ecs-raylib` (`step-n`).
Workaround in both: return `int` with an explicit `0` base case.

Given the narrowing above, there is a cheaper workaround for the non-recursive
case: **move the callee above the caller**. For genuine recursion, `: void`
instead of `: nil` also works and does not require inventing a return value.

## Guides to update when fixed

- docs/guides/type-annotations-guide.md -- if `: nil` and `: void` are meant to
  be interchangeable, say so; this bug is the only thing distinguishing them.
