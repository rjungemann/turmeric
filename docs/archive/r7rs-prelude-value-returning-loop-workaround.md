# Cleanup: write the R7RS prelude's `-lp__` loops as `nil` loops again

> **RESOLVED 2026-09-25.** Every `-lp__` loop in stdlib/r7rs/prelude.tur
> (20, `r7rs-cps-encode-lp__` included) and stdlib/r7rs/read.tur (8) is
> folded back into a single `: nil` function with its original body -- the
> self call renamed, each `true` leaf of the workaround a `nil` -- and the
> wrapper is gone; no caller changed. `tur emit-c` shows each one as a
> `goto __tur_tailcall` loop, and a million-element `string-fill!`,
> `vector-fill!`, `write` of a list, `read` of it back, `read-line` and
> `read-string` pass built at `-O1` (the build that used to overflow) and at
> `-O2`. `r7rs-for-eachn-go__` stays `: bool`: it is CPS and waits on
> [cps-self-tail-call-relies-on-sibling-call](../reported/cps-self-tail-call-relies-on-sibling-call.md),
> which also still overflows a 100,000-element `map` with a lambda at `-O1`
> (pre-existing; measured the same before and after this fold).

**Severity:** low (cleanup; no wrong answer). **Unblocked 2026-09-25:**
[void-self-tail-call-not-lowered](../archive/void-self-tail-call-not-lowered.md)
is resolved, so a `: nil` self tail call is a loop now.

## What and why

r7rs-lang-plan T8 found that a self tail call in a `: nil` function is not
lowered to a loop, so the prelude's statement loops -- fills, copies, port
readers, the printer's list walks -- overflowed the C stack on a long input
in any build where gcc does not make the sibling call (`-O1`, and so every
ASan build).

The workaround gives each loop a value so the existing loop lowering applies:

- the loop is a new `: bool` function named `<name>-lp__`, with `true` at its
  base case and at every leaf, and the recursive call renamed;
- the original `: nil` name is a one-line wrapper, `(defn <name> [...] : nil
  (<name>-lp__ ...) nil)`, so no caller changed;
- a leaf that is a `nil` call was wrapped as `(do <call> true)`, because an
  `if` whose branches disagree joins to `any`, which puts the recursive call
  out of tail position.

That is noise the language should not need. Once a `: nil` self tail call is
a loop, write each one back as a single `: nil` function with its original
body and delete the wrapper.

## Where

`stdlib/r7rs/prelude.tur` (19):

`r7rs-fill-go-lp__`, `r7rs-copy-fwd-lp__`, `r7rs-copy-bwd-lp__`,
`r7rs-vector-fill-go-lp__`, `r7rs-vec-push-range-lp__`,
`r7rs-vec-store-list-lp__`, `r7rs-bytes-push-range-lp__`,
`r7rs-bytes-store-list-lp__`, `r7rs-write-bytes-go-lp__`,
`r7rs-read-line-go-lp__`, `r7rs-read-string-go-lp__`,
`r7rs-cyc-finish-lp__`, `r7rs-cyc-vec-go-lp__`, `r7rs-shr-spine-lp__`,
`r7rs-shr-vec-go-lp__`, `r7rs-pr-tail-lp__`, `r7rs-pr-vec-lp__`,
`r7rs-pr-bytes-lp__`, `r7rs-pr-irritants-lp__`.

`stdlib/r7rs/read.tur` (8):

`r7rs-rd-token-go-lp__`, `r7rs-rd-line-comment-lp__`,
`r7rs-rd-block-comment-lp__`, `r7rs-rd-skip-lp__`,
`r7rs-rd-intraline-lp__`, `r7rs-rd-delimited-go-lp__`,
`r7rs-rd-patch-vec-lp__`, `r7rs-rd-patch-lp__`.

Also `r7rs-for-eachn-go__` (prelude), which T8 wrote as `: bool` for the same
reason. It is a CPS function, so it additionally waits on
[cps-self-tail-call-relies-on-sibling-call](cps-self-tail-call-relies-on-sibling-call.md);
`r7rs-for-each1__` was left `: nil` because the workaround cannot help a CPS
loop.

`grep -n -- '-lp__' stdlib/r7rs/*.tur` finds them all.

## Done when

- Each `-lp__` is folded back into its `: nil` original and the wrapper is
  gone.
- A `-O1` build of every `r7rs-*` fixture still runs (T8's audit script
  builds at `-O1`, and `tests/run-leak-check.sh` builds every opted-in fixture
  there).
- A million-element `string-fill!`, `read-line`, `write` of a list and `read`
  of a list still pass at `-O1` and under `tur --interpret`.
