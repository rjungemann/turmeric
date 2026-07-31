# Non-capturing named let: CPS-colored, self-call recurses through the DK entry wrapper

**RESOLVED 2026-07-30.** Root cause was not the eviction gate this report
guessed at, but call-target RESOLUTION in the coloring analysis: `cps_find_node`
matched only by Binding pointer, and a captureless letrec lambda binds `go` to a
different Binding than the lifted `__fn_N` -- carrying that function's C symbol
in `c_export_name`, which is exactly how the emitter resolves it. The self-call
therefore read as unresolved, set `has_indirect`, and conservatively colored a
loop with no control operator in it. The analysis now falls back to C-symbol
identity, so the loop is uncolored, direct-emitted, and self-TCO'd. See
[jit-engine-j0-findings.md](../upcoming/jit-engine-j0-findings.md) section 31;
pinned by `tests/fixtures/tco-named-let-nocapture-deep`. The original report
follows.

**Severity: medium-high.** A deep loop overflows the stack on EVERY engine
(cc included -- no optimizer can rescue it), and each iteration pays a
`dk_prompt` malloc + `setjmp`. Found 2026-07-30 while fixing
docs/archive/named-let-self-tail-not-tco.md, which turned out to cover only
the CAPTURING form.

## Summary

A named let that captures nothing from its enclosing function lowers very
differently from one that does:

| shape | lowering | self-call |
| --- | --- | --- |
| captures an outer var | lifted closure thunk `__fn_N(void *env, ...)` | now a `__tur_tailcall` backedge (fixed) |
| captures nothing | CPS-COLORED `__fn_N__cps(..., DK *)` | plain call to the DIRECT ENTRY `__fn_N` |

In the second case the body is direct-emitted as a value (the `__ps_N`
temps and the `panic-return-signal` comment are emit_expr's), and the
self-call targets the function's own direct ENTRY WRAPPER -- the one that
bumps `__dk_entry_depth`, `dk_prompt`s a fresh root (a malloc), and
`setjmp`s -- once per iteration:

```c
static int64_t __fn_1330__cps(int64_t i, int64_t acc, DK *__kont) {
    int64_t __t0, __t42;
    if ((i) >= (INT64_C(5000000))) { __t42 = acc; }
    else {
        int64_t __ps_43 = (__fn_1330((i) + (INT64_C(1)), (acc) + (i)));  /* entry wrapper! */
        __t42 = __ps_43;
    }
    __t0 = __t42;
    return dk_run(__kont, (intptr_t)(__t0));
}
__attribute__((unused)) static int64_t __fn_1330(int64_t i, int64_t acc) {
    __dk_entry_depth++;
    DK *__root = dk_prompt(DK_ROOT_TAG, dk_done());
    jmp_buf __dkjb; ...
    if (setjmp(__dkjb) == 0) { __r = __fn_1330__cps(i, acc, __root); }
    ...
}
```

The call is not in tail position from C's point of view either (its result
is bound, then delivered), so unlike the capturing form there is nothing
for gcc's sibling-call optimization to elide: **this one SIGSEGVs on the cc
path too.**

## Repro

```sh
cat > /tmp/nc.tur <<'EOF'
(defn sum-to [] : int
  (let go [i :int 0 acc :int 0]
    (if (>= i 5000000) acc (go (+ i 1) (+ acc i)))))
(defn main [] : int (println (sum-to)) 0)
EOF
tur build /tmp/nc.tur -o /tmp/nc && /tmp/nc      # SIGSEGV
tur --enable=jit jit /tmp/nc.tur                 # SIGSEGV
tur --interpret /tmp/nc.tur                      # 12499997500000 (correct)
```

Add a captured variable (`(defn sum-to [n : int] ...)` with `(>= i n)`) and
the same program now completes on all three engines -- that is the
already-fixed capturing path, and the contrast is the whole diagnosis.

## Root cause

The lambda is colored by the CPS backend even though its body has no
effects, no delimited control, and nothing else the DK machinery exists
for. Coloring it means the direct emitter never sees it, so the CF1
self-TCO path (`tco_mark` / `emit_tail_backedge` in src/compiler/emit_fns.c)
that would lower the self-tail-call to a loop never runs; and the CPS
translation did not classify the self-call as a `cps->cps` tail call, so it
fell back to calling the uncolored entry point -- which for a colored
function is its own re-entry wrapper.

## Fix directions

There is a settled precedent for exactly this: the recursive-await
eviction (docs/archive/cps-async-recursive-await-eviction.md), whose
recorded decision is that a self-recursive shape the DIRECT emitter's TCO
handles in O(1) stack must EVICT from the CPS path rather than be lowered
through DK. See the comments at src/compiler/emit_cps_ir.c:1815 and 2250 --
"strictly worse than the direct TCO path ... this is the settled decision,
not an open gap."

So: extend the eviction gate to a function whose body carries a direct
self-tail-call that `tco_mark` would flag, when nothing else in the body
requires coloring. The function then lands on the direct emitter and gets
the `__tur_tailcall` backedge like every other self-tail loop.

Worth checking while there: whether the `cps->direct` fallback should ever
target a COLORED callee's entry wrapper at all. Re-entering the DK driver
per call is expensive even when the recursion is shallow, so if a colored
self-call cannot be `cps->cps`, evicting is likely better than paying a
prompt per iteration.

## Provenance

Found while fixing the named-let TCO gap (findings 29). The capturing form
is fixed and pinned by `tests/fixtures/tco-named-let-capture-deep`; this
non-capturing form is the remaining half and has a different root cause,
hence a separate report.
