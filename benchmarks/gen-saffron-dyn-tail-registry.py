#!/usr/bin/env python3
"""gen-saffron-dyn-tail-registry -- programs that load T6's bouncer registry.

proper-tail-calls T6 arms a callee by looking up the fat box (or closure
thunk) it is about to call in a registry of BOUNCERS, on every call the
trampoline driver makes.  This writes a Saffron program with N extra
registered bouncers (each boxed as a value, so its box is registered) and one
of two 10,000,000-call loops:

  hit   -- `(run run ...)`: a bouncing loop whose own box is registered LAST.
  miss  -- `(fwd add1 k)` in a static loop: every tail call drives a callee
           that is NOT a bouncer, so the lookup misses both probes.

Usage: gen-saffron-dyn-tail-registry.py N hit|miss   -> writes p_<mode>_<N>.tur
Results: saffron-dyn-tail-results.md ("Registry lookup").
"""
import sys
n=int(sys.argv[1]); mode=sys.argv[2]
out=["#lang saffron"]
for i in range(n):
    out.append(f"(defn b{i} [f k acc] (if (= k 0) acc (f f (- k 1) (+ acc {i}))))")
out.append("(defn run [f k acc] (if (= k 0) acc (f f (- k 1) (+ acc 1))))")
out.append("(defn add1 [x] (+ x 1))")
out.append("(defn fwd [g x] (g x))")
out.append("(defn many [k acc] (if (= k 0) acc (many (- k 1) (+ acc (fwd add1 k)))))")
out.append("(defn main [] : int")
if n:
    for c in range(0, n, 50):
        out.append("  (println (vec-len [" + " ".join(f"b{i}" for i in range(c, min(n, c+50))) + "]))")
if mode=="hit":
    out.append("  (println (run run 10000000 0))")
else:
    out.append("  (println (many 10000000 0))")
out.append("  0)")
open(f"p_{mode}_{n}.tur","w").write("\n".join(out)+"\n")
