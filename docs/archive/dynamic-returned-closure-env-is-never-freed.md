# A capturing closure returned as `any` is never freed

**RESOLVED 2026-09-28**, by two changes that landed the same day:

- **Statically** -- see [Resolution](#resolution-2026-09-28).  A returned
  capturing lambda that a `let` owns is freed at scope exit (or at the DK
  entry boundary in a CPS-lowered caller), with or without a collector.
  Pinned by `tests/fixtures/saffron-returned-closure-env-freed`
  (leak-checked) and `tests/fixtures/saffron-returned-closure-set-not-dropped`;
  `tests/fixtures/tailcall-dyn-leak` lost its `known-leak` marker.
- **By the collector**, for both dialects it names: a compiled `#lang r7rs`
  program has allocated from the r7rs-gc collector since 2026-09-25, and a
  compiled single-unit `#lang saffron` program does too since this date (see
  [any-widen-stored-in-an-adt-field-has-no-owner](any-widen-stored-in-an-adt-field-has-no-owner.md)).
  Measured with the repro in a loop -- 3,000,000 `(make-adder i)` closures --
  the collector ran 11 collections and freed 92 MB with an 8 MB heap.  A
  closure whose owner the compiler cannot prove (stored, returned onward,
  handed to a retaining callee) still has no STATIC owner; that residue is
  [saffron-static-ownership-residue](../reported/saffron-static-ownership-residue.md).

**Severity: low-medium.** One closure env per call that returns a capturing
lambda, in any dynamic file (`#lang saffron`, `#lang r7rs`). No wrong answer,
but a Scheme program is made of exactly this shape -- every `lambda` a
procedure returns -- so a loop that builds closures leaks linearly.

**Pre-existing** at `main` (274f3cbb, measured with the r7rs-lang-plan work
stashed in a separate worktree build). Found at r7rs-lang-plan R9 while
triaging `tur_leak_check`.

## Repro

```turmeric
#lang saffron
(defn make-adder [k] (fn [x] (+ x k)))
(defn main []
  (let [add3 (make-adder 3)]
    (println (add3 4)))
  0)
```

Built with `tests/run-leak-check.sh`'s flags and run with every LeakSanitizer
root disabled (`LSAN_OPTIONS=use_globals=0:use_registers=0:use_stacks=0:use_tls=0`),
the 32-byte env allocated in `make_hyadder` is reported. The typed twin --
`(defn make-adder [k : int] : (fn [int] int) (fn [x : int] : int (+ x k)))` --
reports nothing: the direct emitter frees the env at the `let`'s scope end.
Under default LSan options the dynamic program usually passes too, because a
stale copy of the pointer survives on the stack or in a global and LSan counts
the block reachable. That is luck, not ownership.

## Why it surfaced now

`tests/fixtures/tailcall-dyn-leak` has carried this leak (its `make-counter`
closure) and a second one -- a vector it never freed -- since it was written,
and passed on stale-pointer luck. r7rs-lang-plan R6 changed the dynamic call's
emitted C (the variadic check), which moved those words, and the gate went
red with 168 bytes. R9 frees the vector in the fixture (a compiled Vec is
released with `vec-free`, per docs/guides/gc-guide.md) and marks the fixture
`known-leak` against this report for the closure.

## Where to look

The `let` scope-end drop of a closure env is keyed on the binding's static
type being a fat closure; a dynamic file's binding is `any` (the lambda was
widened on return), so no drop is emitted. `any-widen-stored-in-an-adt-field-has-no-owner`
tracks the same family for by-value payloads.

## Fix directions

- Drop a closure-typed `any` at scope end the way a typed fat closure is
  dropped, when the binding does not escape (the same escape facts the typed
  path uses).
- Or give the `any` box of a closure an owner at the widen, so the box's drop
  releases the env.

Delete `tests/fixtures/tailcall-dyn-leak/known-leak` when this is fixed; the
leak gate turns red if the leak comes back.

## Resolution (2026-09-28)

Four gaps, all of them needed for the repro.

1. **Nothing knew the `any` held a heap env.**  `__tur_any_drop` frees a
   payload box only when the registry row for its tag says `boxed`, and the
   `"fn"` row cannot say that: an `any` holding a function may equally hold a
   static function with no header at all.  The producer knows.  A function
   whose tail widens a capturing lambda built right there now carries
   `returns_fresh_any_closure` (elab_fns.c, beside `returns_fresh_any`).  An
   initializer that is such a call, or such a widen written in place, is
   `expr_is_fresh_any_closure` (emit_core.c).  Its drop is
   `__tur_any_closure_drop`, which releases the env through its drop-glue
   header, and `let_binding_widen_drop_stmt` spells that for it.  The drop is
   admitted only when the env's release frees nothing else
   (`closure_env_drop_is_shallow`): every capture is an `any` or a scalar
   (drop glue leaves those alone), and the body has no inline C (the only way
   a closure's code could hand out its own env).

2. **The escape walk read every dynamic node as an escape.**
   `binding_escapes_impl_x` had no arm for `EX_DYN_CALL`, `EX_DYN_OP` or
   `EX_UNION_INJECT`.  So any `let` whose body made a dynamic call, or widened
   anything, dropped nothing: `(println (add3 4))` widens the call result for
   `println`.  Now a dynamic call whose callee is `b` is an invocation, as the
   `EX_CALL` arm already treated `fn_expr == b`.  The other operands are
   walked, and `b` as an argument to an unknown callee is still an escape.  A
   widen walks its operand.  `(set! b ...)` is now an escape, because the drop
   would free whatever `b` holds last.  That was a use-after-free for every
   scope drop in the family.
   `saffron-returned-closure-set-not-dropped` segfaults without that rule.

3. **Self application.**  `tailcall-dyn-leak` calls `(counter counter 1000
   0)`, which passes the closure to itself.  `fresh_closure_self_apply_mask`
   records, per parameter of the returned lambda, that the lambda only invokes
   that parameter or hands it back to itself in the same slot
   (`any_box_binding_escapes_self_apply`).  The only handle the code then has
   on itself is that parameter, and all it can do is call itself again.  So no
   activation reached from the call can store or return the closure.

4. **A value return from a tail-position `let` fired no `any` drop.**
   `emit_tail`'s inline `let` arm pushes its drops onto the scope list and
   leaves them to "the return".  A backedge and a void return fired them, but
   a value return did not.  Now the value goes into a temp (it may read the
   local), the list fires, and the temp is returned.  This was pre-existing
   for every `any` a tail `let` owned, not only closures.

A CPS-lowered function's `let` is flattened into CPS binders, with no scope
exit to drop at.  There the binder is registered for the DK entry-boundary reap
(`reap_any_env`, `cps_any_closure_env_freeable`), the way `reap_env` already
reaps a typed capturing closure.  The registration is guarded twice.  It runs
only in the function's main body, when `__kont` is not `tur_tb_root`: a
dynamic tail call made with `__kont` equal to the trampoline root bounces its
callee to the driver, which runs it after the entry, and its reap, are done.

`#lang r7rs` is covered by the same code.  A re-entrant `call/cc` could
re-enter a scope after its drop ran, so `__tur_any_closure_drop` returns early
once `tur_dk_pinned` is set (the first capture sets it).  That is the
process-lifetime policy DK memory already takes from that point.  A TU without
the DK runtime cannot have made such a capture, which is what the new
`TUR_DK_PIN` define tells the helper.  Compiled r7rs programs also run under
the conservative collector, which reclaims what the drop skips.

The interpreter is unchanged; its closures are process-lifetime by design.
