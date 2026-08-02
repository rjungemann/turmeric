# Struct field mutation through a parameter: compiled and turi disagree

**Severity: medium.** Not a crash and not a miscompile of either backend on its
own terms -- each is internally consistent -- but the same program prints a
different answer depending on which one runs it, and nothing warns. Any fixture
that mutates a struct through a callee and reads it back in the caller is
asserting one backend's semantics and silently contradicting the other.

Found 2026-08-02 while landing `#writes` frames
(`docs/upcoming/checked-write-frames-plan.md`); unrelated to that work, which
is only how it surfaced.

## Repro

```turmeric
(defstruct Ctr [n : int])

(defn set-it! [^mut a : Ctr] : int
  (set! (.n a) 3)
  0)

(defn peek [^borrow a : Ctr] : int
  (.n a))

(defn main [] : int
  (let [^mut c (Ctr 0)]
    (set-it! c)
    (println (peek c)))
  0)
```

```
$ tur run f.tur          # compiled
0
$ tur --interpret f.tur   # turi tree-walking interpreter
3
```

Note the flag position: `tur run --interpret f.tur` prints `0`, i.e. it does
NOT take the turi path. Only `tur --interpret <file>` (the global flag before
the file, which is what `tests/run-turi.sh` uses) reaches `src/turi/eval.c`.
That is its own small trap -- it cost a wrong "cannot reproduce" during this
investigation.

## Which is right

The compiled answer is the documented one. Turmeric passes by value, and the
compiled backend's `0` follows from that: `c` is copied into `set-it!`, the
write lands on the copy, and main's `c` is untouched. Several analyses already
rest on this -- the landed WF3 slice's "a plain symbol target cannot alias"
rule and WF2's "a bare-symbol assignment to a parameter is not a write" both
cite by-value passing explicitly (`elab_fns.c`, the WF3 header comment).

The interpreter is representing a `defstruct` value as a shared mutable object,
so `(set! (.n a) 3)` writes through to the caller's binding.

## Impact

- **Fixtures**: any `tests/fixtures/` case that mutates a struct through a
  callee and reads it back cannot pass under both harnesses. `run.sh` and
  `run-turi.sh` will disagree, and the failure presents as a plain
  `stdout mismatch` with no hint that a semantic divergence is the cause.
  `wf1-writes-frame-honored` hit exactly this in CI and was rewritten to avoid
  observing the write at all.
- **The refinement analyses are NOT affected.** They are conservative in the
  direction that survives either model: a place-expression write rooted at a
  parameter counts as a write to that parameter in WF2 regardless of whether
  the caller can observe it, and WF3's rescue requires the target to be a plain
  symbol that is never borrowed. A by-reference model makes those rules
  pessimistic, not unsound.

## Fix directions

Two coherent ends, and the choice is a language decision rather than a bug fix:

1. **Make turi match the compiled semantics** -- copy a struct value on
   parameter bind. Correct per the documented model and keeps the analyses'
   premise true in both backends. Cost: a copy per call in the interpreter, and
   it may break interpreter-only code that has come to rely on write-through.
2. **Make the divergence impossible to write** -- reject or warn on a
   place-expression `set!` through a non-reference parameter, on the grounds
   that it is a write nobody can observe in the compiled backend and therefore
   almost certainly not what the author meant. This is attractive independently:
   the compiled behavior is a silent no-op, which is its own footgun.

A third, cheap, non-exclusive step: teach `run-turi.sh` to say something better
than `stdout mismatch` when a fixture's compiled and interpreted outputs differ
but both are internally stable -- the shape is distinctive enough to name.
