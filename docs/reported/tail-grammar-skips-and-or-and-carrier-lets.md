# The tail grammar skips `and`/`or` and a `let` that binds a carrier value

**Severity: low-medium.** These are gaps in what can be written, not wrong
answers. `^tailcall` refuses both shapes with TUR-E0716, so a loop that asks
for the guarantee is told it does not have it. A loop that does not ask
compiles to ordinary recursion. At the default `-O2` gcc turns that into a
loop, and at `-O0` it overflows the stack. The `and`/`or` half is low: an `if`
does the same job. The carrier-`let` half is closer to medium: a loop body
that binds a `Vec` or a list in a `let` is a common shape, and the guide's
"Boundary" list (`docs/guides/performance-guide.md`) does not name it.

Split out of [proper-tail-calls-plan](../archive/proper-tail-calls-plan.md)
T-D4 when that plan was archived on 2026-09-28. T-D4 gave its order as
"`EX_MATCH`, then audit `handle` arms, `and`/`or`, and `tco_let_simple`'s
carrier-ABI bail, each with a fixture before it is relaxed". `match` landed as
T3. The rest of that list was never a stage and was never done; this note is
where it lives now:

- **`and`/`or`**: the last operand is not a tail position (below).
- **`tco_let_simple`'s carrier-ABI bail**: still in place (below).
- **`handle` arms**: out of reach by design. A body containing `handle`
  is CPS-lowered, so `tco_mark`/`emit_tail` never see it (plan TR3). A CPS
  function's self tail call has been a backedge since 2026-09-26. Its
  cross-function tail call is
  [cps-self-tail-call-relies-on-sibling-call](cps-self-tail-call-relies-on-sibling-call.md).

## Repro

All runs used `./build/tur` v0.56.2 (`main` at `1a0c97fad`) with gcc 13.3 on
x86-64 Linux, on 2026-09-28.

### `and`/`or`

```clojure
(defn all-ok? [n : int] : bool
  (and (>= n 0) ^tailcall (all-ok? (- n 1))))
(defn main [] : int (println (all-ok? 3)) 0)
```

```
$ tur check and-tail.tur
and-tail.tur:2:27: error [TUR-E0716]: `^tailcall` call is not in tail position: the call sits in an argument position, and arguments are evaluated before the call they belong to
```

`or` behaves the same:

```clojure
(defn any-hit? [n : int] : bool
  (or (= n 0) ^tailcall (any-hit? (- n 1))))
```

```
or-tail.tur:2:25: error [TUR-E0716]: `^tailcall` call is not in tail position: the call sits in an argument position, and arguments are evaluated before the call they belong to
```

The same loop written as an `if`, `(if (>= n 0) ^tailcall (all-ok? (- n 1))
false)`, passes `tur check`. Without the annotation, the depth test at
10,000,000:

| body | `TUR_CC_FLAGS=-O0 tur build` | default `tur build` (`-O2`) |
| --- | --- | --- |
| `(and (>= n 0) (all-ok? (- n 1)))` | `Segmentation fault`, exit 139 | `false` |
| `(if (>= n 0) (all-ok? (- n 1)) false)` | `false` | `false` |

The emitted C for the `and` body is an ordinary call, with the panic check and
a temp:

```c
static bool all_hyok_qu(int64_t n) {
        bool __t172 = (n) >= (INT64_C(0));
        if (__t172) {
            bool __ps_173 = (all_hyok_qu((n) - (INT64_C(1))));
            if (tur_panicking) return ((bool)0);
            __t172 = __ps_173;
        }
        return __t172;
```

### A `let` that binds a carrier-ABI value

```clojure
(defn count-down [n : int acc : int] : int
  (let [v (vec-new)]
    (if (= n 0) acc ^tailcall (count-down (- n 1) (+ acc (vec-len v))))))
(defn main [] : int (println (count-down 3 0)) 0)
```

```
let-vec-tail.tur:3:31: error [TUR-E0716]: `^tailcall` call is not in tail position: a binding of the enclosing `let` is `fn`-typed, poly-fn, or carrier-ABI, which takes the whole `let` off the tail path
```

Binding `(list 1 2 3)` in place of `(vec-new)` gives the same refusal, even
though the call never uses the binding. Without the annotation, at 1,000,000
steps: `-O0` gives `Segmentation fault` (exit 139) and the default build prints
`0`. A by-value binding is served: `(let [o (some n)] ...)` emits
`__tur_tailcall:` / `goto __tur_tailcall;`.

## Root cause

- **`and`/`or` are builtins, and the tail grammar has no builtin arm.** They
  elaborate to `EX_BUILTIN` with shape `BS_AND_SC` / `BS_OR_SC`
  (`src/compiler/builtins.c:61-62`). `tco_mark`
  (`src/compiler/emit_fns.c:553`) has no `EX_BUILTIN` case, so they fall to
  `default` (line 618) and are "not a tail call". The `^tailcall`
  verifier's `EX_BUILTIN` arm (`emit_fns.c:837-842`) tags every operand
  `TC_ARG`, the last short-circuit operand included. `emit_builtin`
  (`src/compiler/emit_core.c:4640-4661`) lowers the short circuit as
  `bool t = a; if (t) { t = b; }`, which puts the last operand into a temp
  instead of at a `return`.
- **The carrier bail is whole-`let`.** `tco_let_simple`
  (`emit_fns.c:168-176`) returns false when any binding is `fn`-typed,
  poly-fn, or `type_uses_carrier_abi` (`emit_core.c:470`: a `TY_APP`/`TY_ADT`
  carrier such as `Vec`, a `Cons` list, or a parametric heap ADT). That takes
  the whole `let` off the tail path, whether or not the binding is live at
  the call. The verifier reports it as `TC_LET_HARD` (`emit_fns.c:655-657`).
  The bail dates from before `emit_tail` shared its let-binding carrier bridge
  with the ordinary path
  ([tail-recursive-let-drops-carrier-bridge](../archive/tail-recursive-let-drops-carrier-bridge.md)
  made that one function, `emit_let_init_carrier_bridge_type`). Nobody has
  re-checked since then whether the bail is still needed.

`#lang r7rs` does not hit the `and`/`or` half: `and_chain` / `or_chain`
(`src/compiler/scheme_lower.c:2812-2830`) lower Scheme's `and`/`or` to
`let` + `if`, so R7RS's tail-position requirement goes through the `if` arm.
Saffron's `and`/`or` are the lazy `EX_DYN_OP` (`elab_call.c:658`) and were
not probed.

## Fix directions

1. **`and`/`or`.** Treat the LAST operand of a `BS_AND_SC` / `BS_OR_SC`
   builtin as a tail position in `tco_mark`, the `^tailcall` verifier and
   `emit_tail` together (plan risk TR1: the three must agree, or
   `__tur_tailcall:` is emitted unused). In `emit_tail`, lower it the way the
   equivalent `if` already goes:
   `(and a... z)` -> `if (!a) return false; ... <tail z>` and
   `(or a... z)` -> `if (a) return true; ... <tail z>`. Earlier operands stay
   argument positions. The operand types are all `bool`, so T2's
   return-type check has nothing new to do. Pin it with an `-O0` deep
   fixture (`tailcall-and-or-deep`) and a snapshot of the annotated form.
2. **Carrier `let`.** Do the audit T-D4 asked for. Relax the carrier-ABI
   arm of `tco_let_simple` to the bindings `emit_tail`'s `let` arm cannot
   declare, now that it shares the bridge. Write a fixture per carrier kind
   (`Vec`, `Cons` list, a parametric heap ADT) before relaxing each. A
   binding that is an OWNED value live at the call stays refused, for T4's
   reason, and that refusal is already separate (`TC_DROP_LIVE`,
   `emit_fns.c:473`). If some kinds
   must stay refused, add them to the performance guide's Boundary list.
