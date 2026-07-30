# Named-let self-recursion is not emitter-TCO'd; deep loops SIGSEGV under MIR

**Severity: medium.** Expressiveness hole with an engine-dependent blast
radius: silent reliance on gcc on the cc path, SIGSEGV under `tur jit`.
Found 2026-07-30 writing the J3 benchmark triangle.

## Summary

`defn`-level self-tail recursion is rewritten into a loop by the emitter's
TCO (tco-self-tail-deep pins this). Named-let self-recursion is NOT:

```turmeric
(defn sum-to [n : int] : int
  (let go [i :int 0 acc :int 0]
    (if (>= i n)
      acc
      (go (+ i 1) (+ acc i)))))    ; emitted as a real self-CALL
```

The emitted C carries a genuine recursive call for `go`. On the cc path,
gcc -O2's sibling-call optimization happens to turn it into a jump, so the
program works at any depth -- by the grace of an optimization Turmeric never
asked for and c2mir/MIR-gen does not perform. Under `tur jit`, 5,000,000
iterations SIGSEGV on the default 64MB entry stack (~40B/frame -> ~200MB).

## Repro

```sh
cat > /tmp/ls.tur <<'EOF'
(defn sum-to [n : int] : int
  (let go [i :int 0 acc :int 0]
    (if (>= i n) acc (go (+ i 1) (+ acc i)))))
(defn main [] : int (println (sum-to 5000000)) 0)
EOF
tur run /tmp/ls.tur                     # 12499997500000 (gcc sibling calls)
tur --enable=jit jit /tmp/ls.tur        # SIGSEGV
TUR_JIT_STACK_MB=2048 tur --enable=jit jit /tmp/ls.tur   # passes (confirms depth)
```

## Scope

- Any named-let loop whose iteration count scales with input is one engine
  switch away from a crash; on the cc path it is UB-adjacent (unbounded
  stack growth an optimizer happens to elide).
- `-O0`/`-O1` cc builds likely reproduce the overflow on the cc path too
  (unverified; TUR_CC_FLAGS pins -O2 everywhere in-tree).

## Fix directions

Extend the emitter's self-tail TCO to named-let: `(let go ...)` lowers to a
local loop already shaped like the defn TCO's input, so the same rewrite
(params -> mutable locals, self-call -> continue) should apply at the
named-let emission site. Per the standing owner decision (2026-07-29), the
long-run answer is never a bigger stack constant: this belongs with the
stackless/TCO machinery.

## Provenance

J3 benchmark triangle (`benchmarks/triangle/loop-sum.tur`, first version);
the benchmark now uses defn-level self-tail and notes this report.
