# Effect re-opening leaks DK nodes O(N) per re-opened perform

**Severity:** low (memory only; correctness is fine).  Flag-on only
(`--enable=cps-tramp-resume`), and only for a program that uses effect
RE-OPENING (a handler case that performs an effect handled by an ENCLOSING
handler).  Compiled fixture binaries are not ASan/leak-checked by `tests/run.sh`
(they are built with plain `cc`), so this blocks no suite/sweep gate -- but it is
a genuine unbounded leak for a deep re-opening loop.

**One-line:** each re-opened `perform` in a handler case body leaks its
`dk_perform` `sub` chain (~a few DK nodes), so total leaked memory grows O(N) in
the number of re-opened performs executed.

## Repro (valgrind)

```turmeric
;; N re-opened Log performs; the Log case re-opens Write to the outer handler.
(defeffect Log [msg :cstr] :nil)
(defeffect Write [msg :cstr] :nil)
(defn main [] : int
  (handle
    (handle
      (do (perform (Log "1")) (perform (Log "2")) (perform (Log "3")) (perform (Log "4")) 0)
      (Log [msg] k) (do (perform (Write msg)) (resume k 0)))
    (Write [msg] k) (do (println msg) (resume k 0)))
  0)
```

`tur build --enable=cps-tramp-resume` then valgrind:

| re-opened performs | definitely+indirectly lost |
| --- | --- |
| 1 (`Test D`)        | 288 bytes |
| 2 (`effect-reopen`) | 1632 bytes |
| 4 (above)           | 2784 bytes |

Linear in N -> a per-perform leak, not a fixed setup leak.

## Root cause (direction)

The re-opened effect (`Write`) is handled by the OUTER handler, which -- when its
case tail-resumes -- is installed as `dk_handler_tail`.  So the re-opened
`dk_perform` takes the E7 trampoline YIELD branch in `dk_perform`
(`emit_dk_runtime.c`):

```c
if (H->tail_resume && g_dk_driver) {
    DK *__deliv = dk_copy_range(H->next, NULL);
    ... __dk_meta_push(__deliv) ...
    return H->handler(H->handler_env, arg, sub);   /* longjmp -- sub never freed */
}
```

Unlike the non-tramp branch (which does `dk_free(sub)` after the handler
returns), the yield branch returns via `longjmp` to the driver and never frees
`sub` (the `dk_copy_range(k,H)` + appended re-install `tail`).  For a re-opened
perform this happens once per case invocation -> O(N).  The deep E7 fixture
(`cps-tramp-resume-deep`) is valgrind-clean because its resumed chain IS the
driver-run chain (consumed/freed by the trampoline); the re-opening case
allocates an EXTRA `sub` per perform that no one owns.

## Fix direction

Give the yielded `sub` an owner: register it with the reap machinery
(`__dk_reap_node`/`__dk_reap_keep`) at the yield point, or have the driver free
the resumed chain once it settles (mirroring the non-tramp `dk_free(sub)`).  Care
is needed not to double-free the portion the driver re-enters.  Verify with the
table above dropping to 0 lost, and the E7 fixtures staying valgrind-clean.

Discovered while landing the `effect-reopen` DK slice
(`docs/archive/history/cps-perform-cont-heap-join-eviction.md`); the leak is
pre-existing in the re-opening machinery (commit `ffd878897`) and independent of
that slice -- it just first RUNS the re-opening DK path end-to-end.
