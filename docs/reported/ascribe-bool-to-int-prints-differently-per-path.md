# `(:: b :int)` on a bool prints `1`/`0` compiled and `true`/`false` interpreted

**Severity:** low. A both-paths divergence in a narrow spot: an explicit
ascription of a bool to `:int`. The compiled answer is the right one; the
interpreter ignores the ascription for printing purposes.

## Repro

```turmeric
(defn main [] : int
  (println (:: true :int))     ; compiled: 1     interpreted: true
  (println (:: false :int))    ; compiled: 0     interpreted: false
  0)
```

```
$ tur run t.tur
1
0
$ tur --interpret t.tur
true
false
```

## Why it matters

Not for the value -- nothing downstream of the ascription disagrees, only the
rendering. It matters because it silently splits a fixture across the two
paths: a fixture written with `(:: someBool :int)` cannot have one
`expected.stdout` that both harnesses accept, and the mismatch reads as a
product bug in whichever harness you look at second.

Printing the bool without the ascription agrees on both paths (`true`/`false`),
which is the workaround.

## Fix direction

Decide which is right and make the other match. The compiled path is the more
defensible reading -- an ascription to `:int` asked for an int -- so the
interpreter's `println` should consult the ascribed type rather than the
runtime tag of the value it holds. Worth checking whether the same is true for
other scalar ascriptions (`(:: 1 :bool)`, char/int) before fixing just this
pair.

## Found while

Executing
[fixture-dirs-with-loose-tur-files-pass-without-running](../archive/fixture-dirs-with-loose-tur-files-pass-without-running.md),
writing assertions for the revived `tests/fixtures/stm/` fixtures. A TVar's
value is an int64 boxed as `ptr<void>` that `println` will not take, so the
fixtures read values back with `(:: v :int)`; doing the same to the bool from
`tvar/cas` is what surfaced this.
