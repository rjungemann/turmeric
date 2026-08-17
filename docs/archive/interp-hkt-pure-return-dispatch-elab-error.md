# `--interpret` rejects hkt-constrained-pure-return-dispatch that `emit-c` accepts

**RESOLVED 2026-08-17.**  The root-cause direction below was wrong in a way
worth recording: no elaboration mode flag and no result-type pin was
involved.  The failing arg `(mk-box 0)` was not the user's `mk-box` at all
-- the fixture's helper name collides with the stdlib `MapKey` class method
`mk-box` (stdlib/map.tur), and under `--interpret` the bare call resolved to
`__inst_MapKey_mk_hybox_int` (result `:int`), which then failed the `(m
int)` parameter check.  The compiled path resolved the user defn.

Why only the interpreter: `prefer_method_dispatch` (elab_call.c) lets a
class method beat a same-named free defn ONLY for user-defined classes --
`elab_user_method_instance_matches` skips `tc->from_stdlib` to preserve the
documented "user defn overrides a stdlib method" pattern.  But the
interpreter preloads stdlib via `(load ...)` turns that run with
`stdlib_prefix == 0`, outside the `in_stdlib_load` bracket, so every
preloaded typeclass registered with `from_stdlib = false` and MapKey looked
user-defined.  Fixed by `g_turi_stdlib_preload` (runtime/globals.c): the
shared preload helpers (src/turi/preload.c, used by --interpret, the REPL,
and the WASM REPL) and cmd_eval_h's json/schema loads set it for their
duration, and typeclass registration ORs it into `tc->from_stdlib`.
Deliberately NOT applied to binding-level `is_from_stdlib` -- interpreter
fixtures legitimately redefine stdlib defns (the benchmark head/tail stub
pattern), and marking those would newly activate the MF3 collision error
under --interpret.

Also fixed by the same marking: the TUR-W0039 method/defn clash warning no
longer fires spuriously for stdlib classes under the interpreter.  With
this, the ENTIRE hand-run hkt-constrained family passes under `--interpret`
for the first time.  Pinned by
`tests/fixtures/user-defn-shadows-stdlib-method-name/` (inline-C-free, both
engines).

**Severity:** low-medium (one fixture family member; interpreter-only;
pre-existing, surfaced during turi-dict-passing step-4 measurement 2026-08-16)

## Summary

The same elaborator run accepts the program on the compiled path and rejects
it on the interpreter path:

```
$ ./build/tur emit-c tests/fixtures/hkt-constrained-pure-return-dispatch/input.tur   # OK
$ ./build/tur run    tests/fixtures/hkt-constrained-pure-return-dispatch/input.tur   # OK, prints 7 10 3
$ ./build/tur --interpret tests/fixtures/hkt-constrained-pure-return-dispatch/input.tur
input.tur:34:31: error [TUR-E0001]: function 'just-pure' arg 1:
  expected (type-app tyvar 'm' int), got int
```

Verified pre-existing at c909e790 (rebuilt HEAD binary, before the
frame_bind_constraint_dicts change): identical error. The fixture is
PASS-skipped by `tests/run-turi.sh` (TI7 inline-C carve-out), so no harness
reports it; it only shows up when the family is run by hand, which is why
the turi-dict-passing plan prescribes hand-running these.

## Root-cause direction (unverified)

Elaboration is shared, so the divergence must come from a mode difference
the `--interpret` entry point sets up (e.g. HKT by-value carrier defaults or
a different elaboration flag set in cmd handling in src/main.c) rather than
from eval.c. The error shape -- an argument typed `int` failing to unify
with `(m int)` -- suggests the interpreter-path elaboration is not running
the same result-type instantiation/pin that the compiled path applies at
this call (compare `elab_poly_call`'s MB1 expected-type pin, landed
2026-08-16). Start by diffing the elab flags between `cmd_emit_c` and the
interpret command in src/main.c.
