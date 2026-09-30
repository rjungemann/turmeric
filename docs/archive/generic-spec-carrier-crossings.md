# A value typed `A` inside a generic body crossed the carrier at the wrong representation

**Severity: high.** Silent wrong answers, a segfault, and invalid C, in the most
ordinary generic code there is:

```turmeric
(defn first-of [A] [v : (Vec A)] : A (vec-get v 0))
(first-of floats)      ; printed 4.61968e+18 -- 7.1's bits, read as an integer
```

Found 2026-09-30 by the P0 representation-confusion work, by hand-probing float32
across seams after the float-conversion lint went in. **RESOLVED 2026-09-30.**

## Why nothing caught it

The float-conversion lint (`tests/check-emitted-float-conversions.py`) reported
**zero** findings over 2512 fixture programs while this was wrong. The lint does
flag it: `return __ps_178;` is an `IntegralToFloating` cast in a `double`
function. But no fixture had the shape, and neither did any fuzzer. The type
fuzzer only unwraps its wrappers in concrete defns, and it never generates
`float32`. A detector sees only the programs that exist.

So the fix ships with a generator: `tests/generic-spec-matrix.py`, a
deterministic PRODUCER x SINK x TYPE matrix of whole programs. Each cell is
checked compiled, interpreted, and linted.

## Mechanism

A generic defn is emitted as a carrier BASE (every `A` is the int64 word) and
as one SPEC per instantiation (every `A` is the concrete C type). Inside a
spec, every expression typed `A` must be the concrete value, and every crossing
into something still generic must convert at the BITS level. Five independent
paths did not:

| Shape | Before | Root cause |
| --- | --- | --- |
| `(vec-get v 0)` as the body, at float | `4.61968e+18` | `elab_call.c`: a generic call whose result is the enclosing signature's own tyvar was typed `int` in every position. The size-keyed reinterpret wrap cannot size a tyvar and silently dropped itself. The return ladder then did `return <int64 temp>;` into `double`. |
| the same, let-bound, at cstr | segfault | `emit_carrier_bridge` had no arm for pointer-sized leaves: cstr fell to the aggregate deref (`*(const char **)w`), and concrete->carrier spilled the pointer to a stack temp and passed the ADDRESS. |
| `(.val (Box x))`, `x : A` | `4.61968e+18` / `1.088632e+09` | Ctor inference treated an argument typed with the enclosing signature's tyvar as unbound and fell back to the erased bare `Box`, so `.val` was typed `int`. |
| `(match (some (vec-get v 0)) ...)` | invalid C | The same `int` typing inferred `Option__int` for the scrutinee. |
| `(vec-push! w x)`, `x : A` | `3.45846e-323` | No rule in the argument chain bridged a concrete float into an int64 carrier parameter: every rule keys on the elaborated type, which is the tyvar. |

The interpreter had a sibling in the same sweep: `gen-unwrap` at float32 read
the low 32 bits of the box. `yield` had boxed a double's bits, because the
interpreter holds every float kind as a double. It printed `0` for `2.5`.

## Fix

- **Producer, one decision** (`elab_call.c`): such a call is wrapped in the
  `EX_REINTERPRET` typed `A` that the `let` position has used since
  `let-bound-generic-call-result-in-generic-truncates`. The call keeps its
  carrier `int`, and every consumer sees an `A`. Emit lowers it per clone (the
  tyvar arm in `emit_expr.c`): identity in the base, carrier->concrete in a
  spec. It now passes the call's emitted-C note through, so the consumer's
  representation rules see what was emitted.
- **The wrapper is transparent to analyses.** 47 expression walkers across
  elab, emit, CPS coloring, the generator state machine and the van Laarhoven
  monomorphizer handled `EX_ASCRIBE` but not `EX_REINTERPRET`. Each now descends
  through it the way it descends an ascription, so an effect op, yield or drop
  use inside a wrapped call's arguments is still seen. The four return-ladder
  shape predicates deliberately do not descend: to them the wrapper reads as an
  `A`-typed value, which it is. The CPS pass peels it like an ascription. The ABI
  scan registers the wrapped call as the bare call it is, and the site pin never
  reads the carrier `int` under it.
- **Arguments:** passed to a parameter that is itself the carrier word (a
  `val : A` inline-C sink, an unspecialized base), the wrapped call goes in
  verbatim. A concrete float into an int64 carrier parameter is bridged by
  bits, keyed on the two C spellings. A spec minted in a carrier base spells an
  unresolvable bare-tyvar parameter as the carrier.
- **Return:** a `double`/`float` return fed an int64 carrier temp is bridged by
  bits.
- **Ctor inference:** a signature tyvar counts as bound, so `(Box x)` is
  `(Box A)`, which is `Box__float` in the float spec. This is not applied inside
  a `#{Construct}` template, whose bare-ctor body the emitter types from the
  spec's result.
- **`emit_carrier_bridge`:** cstr / `ptr<void>` / sym / int64 cross by cast in
  both directions.
- **Interpreter:** `gen-unwrap` at float32 reads the double it boxed.

## Verified

- `tests/fixtures/generic-spec-carrier-crossings` (compiled and `--interpret`)
  and `tests/fixtures/gen-yield-float32-interp`.
- Full suite green, snapshots regenerated.
- `tests/generic-spec-matrix.py`: see its baseline for the cells still open.
  The lambda sink (`((fn [z : A] : A z) E)` in a float32 spec calls the
  once-lifted carrier thunk through a float-typed pointer) is the M4 calling
  protocol family, tracked in
  `docs/reported/emitted-c-indirect-calls-are-not-type-exact.md`.
