#!/usr/bin/env bash
# region-bracket-lost-when-bt-scope-specializes: a `bt-scope` whose result is a
# by-value record ADT must still OPEN a region -- and must still hand back a
# value that outlives the pop.
#
# Two things this asserts that an output-only fixture cannot:
#
#   1. The bracket EXISTS.  A non-scalar result made the call resolve to an ABI
#      specialization and take the CPS emitter's `/* cps->cps */` arm, which
#      carries no push/pop -- so the program compiled, ran, printed the right
#      answer, and silently never opened a region.  Zero diagnostic, zero
#      output difference.  Only the emitted C shows it, so that is what is
#      grepped.
#
#   2. The value SURVIVES.  This is the SR4 lesson, and the reclamation plan
#      says it applies to regions with more force: a fixture that only checks
#      the program builds would pass with a rewind that reclaimed memory still
#      in use, because the wrong answer is a stale read, not a link error.
#      `mutual` is the case that actually produced one during this work -- a
#      mutually-recursive result whose spine the walk wrongly proved safe,
#      printing 0 instead of 42.
set -u
TMP="$1"
TUR="${TUR:-./build/tur}"

cat > "$TMP/in.tur" <<'EOF'
(load "stdlib/trail.tur")

(defdata Link :heap (Link [v : int nxt : int]))
(defdata RPair (RIP :int :int))

;; Mutually recursive, and NEITHER def is self-recursive -- so the walk's
;; is_self_recursive guard does not see it and the cycle has to be caught by
;; the path check instead.
(defdata MB :copy (MBnil) (MBcons :int :MA))
(defdata MA :copy (MAnil) (MAcons :int :MB))

(defn build [n : int acc : int] : int
  (if (<= n 0) acc (build (- n 1) (:: (Link n acc) :int))))

(defn chain-sum [c : int] : int
  ```c
  struct { int64_t v; int64_t nxt; } *p = (void *)(intptr_t)c;
  int64_t acc = 0;
  while (p) { acc += p->v; p = (void *)(intptr_t)p->nxt; }
  return acc;
  ```)

;; The bracket that used to vanish: a record result, not a scalar.
(defn one-round [n : int] : RPair
  (bt-scope (fn [] (RIP n (chain-sum (build n 0))))))

(defn mkA [n : int] : MA (if (<= n 0) (MAnil) (MAcons n (MBcons n (mkA (- n 1))))))
(defn sumA [a : MA acc : int] : int
  (match a (MAnil) acc
           (MAcons x b) (match b (MBnil) (+ acc x)
                                 (MBcons y r) (sumA r (+ acc (+ x y))))))

;; The result IS the spine, reached through a cycle rather than a self-link.
(defn mutual [n : int] : MA (bt-scope (fn [] (mkA n))))

(defn main [] : int
  (match (one-round 4) (RIP a b) (println (+ a b)))   ;; 4 + 10 = 14
  (println (sumA (mutual 6) 0))                      ;; 2*(1+..+6) = 42
  0)
EOF

if "$TUR" --enable=regions emit-c "$TMP/in.tur" 2>/dev/null \
        | grep -q 'tur_region_push()'; then
    echo "bracket: opened"
else
    echo "bracket: MISSING (the bt-scope emitted no region)"
fi

echo "regions off: $("$TUR" run "$TMP/in.tur" 2>/dev/null | tr '\n' ' ')"
echo "regions on:  $("$TUR" --enable=regions run "$TMP/in.tur" 2>/dev/null | tr '\n' ' ')"
exit 0
