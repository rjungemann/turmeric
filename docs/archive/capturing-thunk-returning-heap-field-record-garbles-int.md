# A capturing thunk returning a record with a `:heap` field garbles its int field (compiled path)

**Severity: medium-high.** A silent wrong answer -- a scalar field reads back
as a nondeterministic, pointer-sized value -- on the compiled path only. No
crash, no diagnostic, no sanitizer report. The interpreter is correct, so it
is also a compiled/interpreted divergence.

**Status:** OPEN. Found 2026-09-05 writing `region-scope-shapes`, the fixture
that pins the second batch of shapes the region escape walk admits. Not
caused by regions: the repro below has no region, no `with-region`, no flag.

## Repro

```turmeric
(defdata Link :heap (Link [v : int nxt : int]))
(defdata HoldsLink (HL :int :Link))
(defdata HoldsInt  (HI :int :int))

;; The shape bt-scope / with-region use: a generic bracket over a fat thunk.
(defn ident [A] [^fat body : (fn [] A)] : A (body))

;; The thunk CAPTURES n.
(defn cap-heap [n : int] : HoldsLink (ident (fn [] (HL n (Link n 0)))))
(defn cap-int  [n : int] : HoldsInt  (ident (fn [] (HI n n))))

(defn main [] : int
  (match (cap-heap 7) (HL a l) (println a))   ;; expect 7
  (match (cap-int 7)  (HI a b) (println a))   ;; expect 7 -- control
  0)
```

```
$ tur run repro.tur
93996078461920      <- garbage, and different on every run (94897924379616 next)
7
$ tur --interpret repro.tur
7
7
```

## What the four variables say

Each was isolated against the others; only the marked combination fails.

| thunk captures? | second field | compiled | interpreted |
|---|---|---|---|
| no  | `:heap Link` | 7 (correct) | 7 |
| yes | `:int`       | 7 (correct) | 7 |
| **yes** | **`:heap Link`** | **garbage** | 7 |

So it needs BOTH a capturing closure AND a `:heap` ADT field in the returned
record. An all-scalar record through the same capturing thunk is fine; a
heap-field record through a NON-capturing thunk is fine. A record with a
`:float` or `:cstr` second field (the fixture's other shapes) is also fine.

The garbage values are heap-address-sized, and they are the int field `a`
reading wrong -- the pointer field `l` is not even touched in the failing line.
That is consistent with the two slots being read in the wrong order, or the
record being read back through a boxed-pointer path (the box's address landing
in slot 0) rather than by value.

## Root cause -- not established

A `:heap` field is a typed-pointer carrier (CONV-S1 seam 3: `record_full` is
true for `is_heap`, so the field's `full_type` records the `Link` type and the
slot is the pointer, not an inline aggregate). The plausible mechanism is that
this changes how `HoldsLink` is classified when it flows back through the
capturing closure's return path -- an `int + pointer` record taking a different
repr / spill / readback than an `int + int` or `int + float` one -- and the
match binder then reads the wrong word. The non-capturing case working points
at the capturing closure's env / fat-return bridge specifically, not at the
record layout itself (which `direct` construction reads correctly).

Where to look: the fat-closure return path for a by-value aggregate result
(the `__tur_aggrspill` / b4box conventions the SR plans describe), gated on
whether the aggregate contains a typed-pointer (`:heap`) field.

## Why it matters for regions

`bt-scope` and `with-region` are exactly this bracket shape, and a bracket
whose body captures its inputs is the normal case. So a program that returns
a record holding a `:heap` handle out of either bracket gets a wrong answer
today -- regardless of the region, which correctly RETIRES such a result (it
reaches a node) and so never rewinds it. `region-scope-shapes` keeps its
`r-heap` shape DEFINED so the retire is counted, but deliberately does not run
it, because running it would bake a nondeterministic pointer into the
expected output. When this is fixed, that call can be restored to `main` and
its value (10) asserted like the other three.

## Resolved 2026-09-05

**Neither variable in the table above is the cause.** The capture only decides
which emit path the caller takes, and the `:heap` field only decides that the
record is heap-boxed rather than by value. What the table could not see is that
the repro's CONTROL is what breaks the subject: delete `cap-int` and `cap-heap`
was always right.

**Root cause.** The direct path's by-args spec matchers (`find_matched_abi_spec`
in emit_expr.c and the fallback loop in `emit_call_name`, emit_core.c) resolved
`cap-heap`'s call to `ident__spec__tur_adt_HoldsInt_int64_t` -- the clone
minted for the OTHER instantiation. `ident`'s type variable reaches only the
result, so every instantiation presents the same erased fat-thunk argument, and
the one result guard (`emit_spec_result_mismatch`) rejected a mismatch only
between two distinct PRIMITIVE kinds. `HoldsLink` and `HoldsInt` are both
`TY_ADT`, so the guard passed; but `HoldsLink` c-names to the `int64_t` carrier
(a heap-boxed record) while `HoldsInt` is a 16-byte by-value aggregate. The
clone called the thunk through `tur_thunk_tur_adt_HoldsInt_t`, took the box
POINTER as the first word of a `HoldsInt`, and `cap_hyheap` then boxed that
aggregate again and returned it as the `HoldsLink` carrier: the match read the
address where the int belonged. `cap-int` printed correctly for a different
reason -- it is CPS-lowered, where
[cps-call-arm-ignores-abi-specialization](cps-call-arm-ignores-abi-specialization.md)
had already made the result type discriminate. That report's "the direct path
consults [the registry] correctly" was true for its float-vs-record repro
(distinct kinds) and false for this ADT-vs-ADT one.

**Fix.** `emit_spec_result_mismatch` now takes the ctx and, after the primitive
rule, rejects a spec whose result C spelling differs from the call's -- when
BOTH results are decisive (not a bare type variable, not unknown, not an app
with an abstract head; those c-name to the carrier exactly as a genuine `int`
does, and narrowing on them would reject the right clone for a call the
enclosing spec has not grounded). A pure narrowing, the same shape as the CPS
side's `spec_result_matches`: it can only turn a wrong hit into the erased
base (which is what a carrier-riding result wants) or an ambiguity into the
unique right hit. All three direct-path matchers -- both real ones and the
`--emit-abi-trace` mirror, which had drifted from even the primitive guard --
consult the one predicate.

**Pinned** by `tests/fixtures/spec-selected-by-result-type-direct` (values
READ, including the heap field through the fixed path), and the `r-heap` shape
in `region-scope-shapes` is restored to `main` with its value asserted, as this
report said it should be. Both `cps-spec-*` fixtures and
`region-scope-adt-result` unchanged.
