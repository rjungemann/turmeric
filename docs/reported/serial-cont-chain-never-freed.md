# A serial-shift receiver's continuation chain is never freed

**Severity: low (leak; one chain per capture).**  Filed 2026-10-03, found
while leak-checking the fixtures for
[serial-receiver-effect-under-if-closure-or-leaf](serial-receiver-effect-under-if-closure-or-leaf.md).
Pre-existing on `main`, on the native serial lowering and the outward one alike.

## Repro

```turmeric
(load "stdlib/serial.tur")
(defn page [env : cstr hole : int] : int (+ hole 1))
(defn recv [k : serial-cont] : int (k 41))
(defn run [] : int
  (serial-reset (page "" (serial-shift recv 0))))
(defn main [] : int
  (println (run))
  (println (run))
  0)
```

Built the way `tests/run-leak-check.sh` builds (`TUR_RUNTIME=source`,
`-fsanitize=address`, `detect_leaks=1`):

```
SUMMARY: AddressSanitizer: 720 byte(s) leaked in 6 allocation(s).
Direct leak of 240 byte(s) in 2 object(s) ... in dk_new
Indirect leak of 480 byte(s) in 4 object(s) ... in dk_new
```

Three `DK` nodes per capture: the copied prompt, the `page` frame, and the
`dk_done` tail.  The existing fixtures show the same: `serial-shift-colored-receiver`
leaks 52 allocations, `cps-oracle-serial-closure-recv` 25,
`serial-shift-receiver-effect-reaches-handler` 12.  None of them carries
`requires.leak-check`, which is why nothing reports it.

## Root cause

The shift body hands the receiver a COPY of the captured chain
(`DK *__cap = dk_copy_range(subk, NULL)`, `run_skbody<N>` in the emitted C), and
the outward lowering hands it the freshly built chain (`emit_serial_outward_call`);
"the receiver owns `dv`" either way.  But a `serial-cont` is multi-shot and
marshalable: `tur_serial_cont_resume` is `dk_invoke(k, v)`, which runs a copy
and leaves `k` intact, because the receiver may resume it again, serialize it,
or store it.  So no use of `k` is a last use the compiler can see, and nothing
ever frees the chain.

## Fix directions

1. **Free on the receiver's return when `k` cannot have escaped.**  The
   receiver's body is visible; if every use of `k` is a direct
   `(k v)` / `resume-cont!` / `serialize-cont` call and `k` is not stored,
   returned or captured, the shift body (or the outward call's resume frame)
   can `dk_free` the chain after the receiver returns.  That covers every
   fixture above.
2. **Give `serial-cont` an owner type.**  A linear/affine continuation
   (consumed by `resume`, cloned explicitly) makes the last use syntactic.  A
   bigger change to a surface that is documented as aspirational
   ([serializable-continuations-aspirational-surface](../archive/serializable-continuations-aspirational-surface.md)).

Either way a fixture with `requires.leak-check` belongs with the fix.
