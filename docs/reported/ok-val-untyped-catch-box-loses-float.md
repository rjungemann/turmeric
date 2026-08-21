# `ok-val` on an UNTYPED catch box truncates a float payload

**Severity: low-medium** (silent wrong number, narrow shape). Found 2026-08-21
while fixing the float half of
[catch-unwind-aggregate-return-miscompiled](../archive/catch-unwind-aggregate-return-miscompiled.md);
that fix makes the ANNOTATED shape correct and leaves this one wrong.

## Repro

```turmeric
;; annotated: correct
(defn f [] : (Result float int) (catch-unwind (fn [] : float 7.5)))
(println (ok-val (f)))                       ; => 7.5

;; unannotated: wrong
(let [r (catch-unwind (fn [] : float 7.5))]
  (println (ok-val r)))                      ; => 4.62013e+18
```

Before the aggregate/float fix both printed `0`, so this is not a regression in
correctness -- the wrong answer just stopped looking like a plausible zero.

## Root cause

The two shapes go through different consumers of the same box.

- With `(Result float int)` in the type, the emitter builds the typed monomorph
  and reinterprets the payload:
  `.ok_val = ((union { int64_t s; double d; }){.s = __box->ok_val}).d`
  (`emit_core.c:4392`), which is what the box carries.
- Without it, `(ok-val r)` monomorphises against the raw `int64_t` box and
  emits `ok_val__spec__double_int64_t`, whose whole body is
  `return (int64_t)((tur_adt_Result *)(intptr_t)(r))->ok_val;` -- an INTEGER
  read implicitly converted to `double`. There is no shape of the box that
  makes that right: it converts the bit pattern numerically.

So the box's contract ("a float payload rides as its bits") is honoured by one
consumer and not the other.

## Fix direction

Make the untyped spec reinterpret like the typed one: when a `Result`/`Option`
accessor monomorph's payload type is a float kind and the source is the raw
carrier box, emit the same union read instead of an integer read. The site is
the mono-spec body generator for the untyped-box accessors, not the typed
construction path (which is already correct).

Worth checking in the same pass: `err-val` over a float err payload, and
`option`'s `unwrap-or-carrier` (the same int64-carrier read).

## Adjacent, not the same

A `bool` payload survives the compiled path but prints `1` rather than `true`
under `--interpret`: the interpreter's Result box keeps int/bool payloads as
bare int64 (`turi_ok_result_box`), so the TURI_BOOL tag is lost and `println`
picks the int overload. That is the display divergence family archived as
`ascribe-bool-to-int-prints-differently-per-path`, not this truncation.

## Guides to update when fixed

- none (no guide documents the untyped-box accessor shape)
