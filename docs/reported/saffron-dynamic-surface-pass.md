# Saffron dynamic-surface pass -- findings (2026-09-09)

**Status 2026-09-10.** Resolved on this branch: H1, H2, H3, H4, H5, H6, H8
(named functions), H9 (a sibling determines K), H10, H11, M3, M4, M5, M6, M8,
M10, each pinned by a fixture and retired from the fuzzer's KNOWN table. M2 is
resolved on the INTERPRETER and root-caused (not fixed) compiled, and M7's hard
error is fixed (the cons-list WALK is not). Open: H7 (fn-typed seam, a
representation gap), M1, M2 (compiled half), M7 (the erased self-referential
tail), M9, and the lows. Struck-through items below carry their resolution
note.

THREE of those -- H7, M1, and M2's compiled half -- are one SHAPE of problem: a
value whose Saffron representation (a 16-byte `tur_tagged_t`) does not fit the
representation the typed path already chose for it (a `tur_poly_fn_t` for H7,
an `int64_t` carrier payload/return for M1 and M2). None is a gate or a keying
bug; in each the cheap-looking fix produces a SILENT WRONG ANSWER rather than a
panic (M1 already does -- it truncates a 16-byte lambda return through an
`int64_t` function pointer), which is why the other two are parked behind a
clean decline. They want to be picked up together, and M1's and M2's filed fix
directions have both been corrected in place after being measured against the
emitted C.

**Summary.** A differential pass (compiled `tur run` vs `tur --interpret`,
Debug build at b58c91e6) over the Saffron surface described in
`docs/guides/saffron-guide.md`. Every probe is a `#lang saffron` file unless
marked *typed*. Grouped by severity. Each item is one bug; split into
per-slug reports as they are picked up. The pass started from the guide's
claim that "a typeclass method on an `any` key is not dispatchable yet":
that sentence is stale (`Hash[any]`/`MapKey[any]` landed in a9ffb842 the day
after it was written, and `(.hash x)` on an `any` dispatches on both back
ends) but the scenario behind it is broken worse than it says -- see H2/H3.

## High -- crash, uncompilable C, or silent wrong answer

**~~H1~~. RESOLVED 2026-09-10: `collect_free_vars` (elab_core.c) had no arms for the Saffron nodes (`EX_DYN_OP` / `EX_DYN_CALL` / `EX_DYN_FIELD` / `EX_DYN_METHOD`), so a variable referenced only inside one was never captured; both traversals now descend into them. Pinned by `tests/fixtures/saffron-capturing-lambda`; KNOWN row retired. Was: a capturing lambda in a Saffron file loses its environment (both back ends).** The canonical closure does not work:

```turmeric
(defn adder [k] (fn [x] (+ x k)))
(let [f (adder 7.1)] (println (f 1)))
;; compiled: cc error `'k_1495' undeclared` -- the lambda is emitted as a
;;           plain `static tur_tagged_t __fn(tur_tagged_t x)` with `k` free
;; interp:   tur: unbound variable: k
```

Same with a let-bound capture (`kk`), a typed `[k : int]` outer parameter,
and through a dynamic call. Only a non-capturing lambda works, which is all
`saffron-higher-order` and the guide's `apply-twice` exercise. Env building
is `collect_free_vars` at `src/compiler/elab_fns.c:10226`; the Saffron
return-widen/unbox paths at `elab_fns.c:8171` and `:8234` sit in the same
elaboration and are the first suspects. The interpreter failing identically
says the loss is in elaboration, not emit.

**~~H2~~. RESOLVED 2026-09-10: a miss on a `(Map K any)` is the nil box on both back ends (`emit_core.c` carrier->any deref, interpreter `map_val_read`), so `(if (map-get m k) ...)` is a presence test. Pinned by `tests/fixtures/map-get-miss-on-any-value-is-nil`. Was: `map-get` miss on a `(Map K any)` segfaults compiled (typed too).**

```turmeric
(let [m0 : (Map int any) (map-new)  m1 (map-assoc m0 1 (:: 7.1 any))]
  (println (map-has? m1 2))          ;; false
  (println (type-of (map-get m1 2))))  ;; compiled: Segmentation fault; interp: int
```

`map-get` returns 0 on a miss and the emitted read derefs it as a 16-byte
`tur_tagged_t` (`*(tur_tagged_t *)(intptr_t)0`). `#map{...}` in Saffron is
always `(Map Sym any)`, so every Saffron miss is this. `saffron-map-literal`
only does hits.

**~~H3~~. RESOLVED 2026-09-10 by H4 + H2: with the Sym arm, `Hash[any]` / `MapKey[any]` on a keyword key agree with `Hash[Sym]` / `MapKey[Sym]` (hash, carrier, comparator), so an `any` key hits the same entry a typed key does, and a key whose payload type does not match the map's is a nil miss rather than a crash. Pinned by `tests/fixtures/saffron-any-key-into-typed-map`. Was: an `any` key into a concretely-keyed map: segfault compiled, silent miss interpreted, no diagnostic.**

```turmeric
(defn lookup [m : (Map Sym any) k] (map-get m k))
(lookup #map{:a 1 :b "two"} :a)   ;; compiled: segfault (H2 on the miss); interp: 0
```

The key passes `tur-map-kcheck` (`stdlib/map.tur:500`) as `(& K)` so no
seam narrowing fires; `hash`/`mk-box`/`mk-cmp` then resolve on the key's
static type `any` -> `Hash[any]`, which hashes a Sym by type name, never
matching the `Hash[Sym]`-hashed entries.

**~~H4~~. RESOLVED 2026-09-10: both instances gained a `Sym` arm (needs H5 for the interpreter's `is?`). Pinned by `tests/fixtures/saffron-sym-in-any`. Was: `Hash[any]` / `MapKey[any]` have no `Sym` arm: keyword keys collapse compiled.** `stdlib/typeclass-hash.tur:84`, `stdlib/map.tur:435`.

```turmeric
(println (set-count #set{:a :b :c}))   ;; compiled: 1   interp: 3
;; same with (Map any int) keyed by (:: :a any) / (:: :b any): count 1, second overwrites first
```

**~~H5~~. RESOLVED 2026-09-10: the compiled dynamic runtime gained a Sym row (`TUR_DYNTAG_SYM`, `type-of` "Sym", `=`/`not=` by pointer identity, the panic message names Sym) and the interpreter boxes a widened Sym under the name "Sym" like a named type, with the dynamic operator layer answering `=`/`not=` and refusing the rest by name. Pinned by `tests/fixtures/saffron-sym-in-any`; KNOWN row retired and `sym` joined the fuzzer's default scalar pool. Was: a Sym inside an `any` is mis-tagged (typed Turmeric too).**

```turmeric
(defn k [x] (type-of x))  (k :kw)        ;; compiled: "unknown"  interp: "int"
(defn s [x] (is? x Sym))  (s :kw)        ;; compiled: true       interp: false
(defn eq [a b] (= a b))   (eq :a :a)     ;; compiled: panic `=: no operator`  interp: true
(defn p [x] (println x))  (p :kw)        ;; compiled: panic   interp: prints a raw pointer
```

`(cast x Sym)` works on both. The compiled name registry answers "unknown"
(`emit_module.c:10616`); the interpreter tags a Sym as int. H4's `is?`
chain cannot be fixed for the interpreter until this is.

**~~H6~~. RESOLVED 2026-09-10: pass 1 (`elab_pre_declare_toplevel_defn`, `fwd_decl_scan_params`) now applies the Saffron default to the forward decl itself: return `any` except zero-arity `main`, unannotated params `any`. Pinned by `tests/fixtures/saffron-forward-reference-defaults-any`; KNOWN row retired. Was: forward reference to a later unannotated defn defaults its return (and params) to `int`, not `any`.**

```turmeric
(defn user [] (+ (later) 1))
(defn later [] 7.1)
;; compiled: cc error `invalid operands to binary +` (tur_tagged_t + long)   interp: 8.1
(defn even? [n] (if (= n 0) true (odd? (- n 1))))   ;; static: then=bool else=int
(defn odd?  [n] (if (= n 0) false (even? (- n 1))))
```

Self-recursion is fine (pinned by the `main`-signature note at
`elab_fns.c:8171`); the forward decl of a *different* not-yet-elaborated fn
still gets the typed default.

**~~H7~~. RESOLVED 2026-09-10 on BOTH back ends (the report recorded only the
compiled half; the interpreter panicked too, `cast: any holds fn, not (fn [int]
: int)`).** The seam into a typed function parameter now synthesises a
MARSHALLING adaptor rather than reinterpreting the payload word:

```turmeric
(let [__sfn <the any-valued argument>]
  (fn [__sa0 : A0 ...] : R (cast (__sfn __sa0 ...) R)))
```

built in `saffron_seam_fn_adaptor` (elab_call.c) beside its outbound twin
`saffron_dyn_fn_adaptor`. `(__sfn __sa0 ...)` is a dynamic call on an `any`
head, which boxes each argument and yields an `any`; the `cast` unboxes the
result to `R` with the ordinary checked-cast panic if the function returned
something else. The `let` is load-bearing -- without it the argument expression
would be re-evaluated on every call of the adaptor. Because it is a SOURCE-level
construct, one change fixes both back ends. Pinned by
`tests/fixtures/saffron-seam-into-typed-fn-param`; the `KNOWN` row and the
known-probe are both retired, and 250 fuzz cases with the shape back in the
default pool are clean.

**The filed fix direction was wrong, and measurably so.** It proposed accepting
the fat id at the compiled `EX_ANY_CAST` and building the `tur_poly_fn_t` from
the fat box, limited to captureless lambdas. That COMPILES and is a silent
wrong answer: H8's outbound adaptor makes every function that enters an `any`
an all-`any` one, so the fat box's slot 0 is a
`tur_tagged_t (*)(void *, tur_tagged_t)` shim whatever the original signature
was -- and the callee would then call it with raw machine words where 16-byte
tagged values are expected. A representation this different needs marshalling,
not a reinterpret. The captureless restriction also turns out to be unnecessary:
going through the BOX rather than a static wrapper means a CAPTURING closure
(`(mk 10)`) round-trips like any other, which the fixture pins.

**A separate finding came out from under this one.** The adaptor declines an
all-`any` target -- correctly, since that is already the representation the box
holds -- and such a parameter turns out to be unusable on its own account: the
callee's body does not compile (`called object 'f' is not a function`, the
parameter emitted as a bare `int64_t`). Filed as
[all-any-fn-param-is-unusable](all-any-fn-param-is-unusable.md); lifting the
decline was measured NOT to fix it. Was:

```turmeric
(defn tfn [f : (fn [int] int)] : int (f 1))
(defn id [x] x)
(tfn (id (fn [x : int] : int (+ x 1))))
;; compiled: cc error `incompatible type for argument 1 of 'tfn'`   interp: 2
```

**~~H8~~. RESOLVED 2026-09-10 for a NAMED typed function: the widen wraps it in an all-`any` adaptor lambda `(fn [__da0 ...] (NAME __da0 ...))` (`saffron_dyn_fn_adaptor` in elab_call.c), whose parameters take the dialect default, whose inner call goes through the checked seam, and whose return takes the `any` pin. Pinned by `tests/fixtures/saffron-typed-defn-in-any`; KNOWN row retired. **Residual:** a typed closure produced by an expression (`(app (adder 7) 1)` with a typed `adder`) is not named and still panics. Was: a typed defn or typed capturing closure held in an `any` cannot be called compiled.**

```turmeric
(defn app [f x] (f x))
(defn inc-typed [n : int] : int (+ n 1))
(app inc-typed 41)
;; compiled: panic `cannot call this function here -- it takes a different number
;;           of arguments, or parameters this call site cannot supply`   interp: 42
```

The guide says a function value is "just another thing an `any` can hold".
Message site `emit_module.c:10078`.

**~~H9~~. RESOLVED 2026-09-10 for the shape where a sibling argument determines the key type: a borrow type now carries its type variable's name (`ref_borrow.target_tyvar`, set by the `(& K)` annotation parse), the binding collector binds through it, and the seam pre-binds its target from the call's other arguments (elaborating a simple later sibling early, as the bidirectional-inference path already does) before grounding only the genuinely open ones. Pinned by `tests/fixtures/saffron-any-map-into-map-get`; KNOWN row retired. **Residual:** an `any` map with an `any` key (`(defn put [m k v] (map-assoc m k v))`) still panics `different instantiation of Map` compiled: nothing determines K statically, and a head-only match would let a `(Map Sym int)` be read as `(Map any any)`. The interpreter, which has no instantiation to check, answers. Was: an `any`-held map into `map-get` / `map-assoc` panics compiled.**

```turmeric
(defn lookup [m k : Sym] (map-get m k))   ;; compiled: `cast: any holds a different
(lookup #map{:a 1} :a)                    ;;  instantiation of Map`   interp: 1
```

The seam grounds both type params to `any` -> expects `(Map any any)`
instead of binding K from the key. Map twin of the archived
`saffron-unannotated-param-container-cast-panics`; no seam fixture uses a
map.

**~~H10~~. RESOLVED 2026-09-10: `elab_fn` pins an unannotated Saffron lambda's return to `any` and boxes the body (unless an expected fn type decides it, or the body is nil), mirroring `elab_defn`; the explicit `: any` case gets the same widen. Pinned by `tests/fixtures/saffron-lambda-literal-body-dyn-call`; KNOWN row retired. Was: a lambda whose body has a concrete type is not dynamically callable compiled.** Saffron's `any` return default reaches `defn` but not `fn`:

```turmeric
(defn call0 [f] (f))
(call0 (fn [] 7.25))          ;; compiled: panic `cannot call this function here --
                              ;;   it takes a different number of arguments ...`   interp: 7.25
(call0 (fn [] : any 7.25))    ;; compiled: cc error `returning 'double' but
                              ;;   'tur_tagged_t' was expected`
(app (fn [x] "s") 1)          ;; same panic; (fn [x] x) and (fn [x] (+ x 1)) work
```

A lambda body that flows through an `any` (`(fn [] (t 7.25))`) is fine, so
the literal-bodied lambda gets a `(fn [] float)` signature the dynamic call
site refuses. Same message site as H8.

**~~H11~~. RESOLVED 2026-09-10: the ABI scan now notes every candidate field type of a dynamic field read as a widened tag (`emit_module.c`, `EX_DYN_FIELD`), so the instance rows are published. Pinned by `tests/fixtures/saffron-dyn-field-read-dispatch`; KNOWN row retired from the fuzzer. Was: a value read out of a typed struct field cannot dispatch a typeclass method unless something else in the file widened that type.** Found by
`tests/saffron-fuzz-src.py` (seed 7, eight cases, all `wrap_struct` +
`term_class`).

```turmeric
(defclass Kind [a] (kind-of [x] : cstr))
(definstance Kind [float] (kind-of [x] "F"))
(defstruct Pf [fld : float])
(defn k [x] (.kind-of x))
(defn get [p] (.fld p))
(k (get (make-struct Pf 7.25)))   ;; compiled: panic `no instance of Kind for float
                                  ;;   (dispatching .kind-of on an any)`   interp: F
```

`(type-of ...)` on the same value says `float`, and adding an unrelated
`(t 1.5)` anywhere in the file makes the dispatch succeed. The instance
registry is populated per WIDENED tag (`emit_module.c:6913`, the tag axis),
and the dynamic field read (`emit_expr.c:6338` area) is a widen site that
does not add its field type to that set. A vec element read does.

## Medium -- back-end divergence or documented-surface hole

**~~M1~~. RESOLVED 2026-09-10 on both back ends: `(type-of (half (some 7.1)))`
is `Option` and the value is `3.55`, which is what the interpreter always
answered.** The difference between `.bind` and the `.fmap` that always worked
is one line of their DECLARATIONS -- `Functor`'s `g : (fn [a] b)` returns a
bare type variable, `Monad`'s `k : (fn [a] (m b))` returns the class variable
APPLIED. The D8 witness cast every erased fn extra to `(fn [any] any)`; for
`fmap` that is right and grounds `b := any`, but for `bind` `(m b)` cannot
unify with a bare `any`, so the result instantiation never grounded and `.bind`
fell back to the erased carrier
`__inst_Monad_bind_Option(int64_t, tur_poly_fn_t)`. The witness now passes a
MARSHALLING adaptor for such a continuation -- the same move H7 makes at the
argument seam --
`(fn [__ka : any] : (Option any) (cast (__ak __ka) (Option any)))` -- so `k`
really is `(fn [any] (Option any))`, `b := any`, the instantiation is concrete,
and the ABI scan mints the by-value spec. Pinned by
`tests/fixtures/saffron-dyn-bind-on-any-option`, which also pins the `fmap`
twin so a later witness change cannot fix one by breaking the other.

**The filed fix direction was wrong twice over**, which the corrected note of
2026-09-10 had already established and this fix confirms: the body was not an
`any` being re-widened, and the defect was a TRUNCATION rather than a nesting
(the carrier calls the continuation through
`((int64_t (*)(void*, int64_t))k.fn)` while a Saffron lambda returns a 16-byte
`tur_tagged_t`, so half the box was dropped on the return).


**M2. `Functor[Result]` is not dynamically dispatchable COMPILED.** *Update
2026-09-10: the interpreter half is fixed; the compiled half is a
representation gap, root-caused below -- not the keying this entry originally
guessed.* `(.fmap (ok 21) f)` via `any` was `no instance of Functor for Result`
on both back ends, while Option dispatches.

Root cause, common to both back ends: `Option`'s instance head is the bare
constructor (`definstance Functor [Option]`, a `TY_ADT`), but Result's is
written PARTIALLY APPLIED -- `definstance Functor [(Result _ B)]`
(`stdlib/result.tur:310`) -- which is a `TY_APP` chain. The interpreter
compared `type_name(inst->type_args[0])` against the name the box reports, and
`type_name` spells a chain `(type-app (type-app Result ?) B)`, never the
`Result` that the receiver's own `type-of` reports, so the row could never
match. Peeling the chain to its head makes the two spellings agree
(`src/turi/eval.c`, `EX_DYN_METHOD`): interp now answers 42 and `type-of` says
`Result`. Pinned by `tests/fixtures/saffron-dyn-hkt-partial-head`
(`requires.interp-only`, since the compiled path still declines).

**Still open, compiled. Root cause narrowed to a LAYOUT mismatch (2026-09-10),
which settles that no keying or adaptor can reach it.** A hole-headed instance
is compiled ONCE, generically: `inst_type_suffix` spells the open parameter
`tyvar`, giving a single `__inst_Functor_fmap_Result_tyvar`, and no Result
`fmap` is ever specialised -- the TYPED path with a fully concrete
`(Result int int)` uses that same carrier. The two Result layouts in one
emitted TU are:

```c
typedef struct tur_adt_Result        { int tag; union { struct { int64_t     _0; } Ok; ... } as; };
typedef struct tur_adt_Result__any__any { int tag; union { struct { tur_tagged_t _0; } Ok; ... } as; };
```

Different payload SIZES, so different offsets. A witness minted on the peeled
head hands `&tur_adt_Result__any__any` to a carrier that reads it as
`tur_adt_Result *`, so the receiver is misread before the continuation is even
called. That is why the M1 fix does not carry over: M1 could repair its
continuation's calling convention from source, and there is no source-level
spelling that converts a container's payload representation. The fix is a
by-value spec for the hole-headed instance at its all-`any` instantiation --
i.e. teaching the monomorphiser to clone it -- and nothing short of that.

**The chain from cause to symptom, measured end to end (2026-09-10), so the
next attempt starts where the evidence stops rather than at the keying it
already ruled out:**

1. `definstance` with a partially-applied head records the receiver as a
   `TY_APP` and FORCES the parameter to the int64 carrier -- the `T4` note at
   `elab_typeclasses.c` (`"a partially-applied instance head (e.g. `(Result _
   B)` / `(Either E)`) ... Force the carrier here too"`). So
   `__inst_Functor_fmap_Result_tyvar`'s declared signature carries no named
   type variables at all.
2. A call site therefore collects no bindings for it.
   `emit_abi_register_call` sees `__inst_Functor_fmap_Result_tyvar` with
   **nb=0**, where the working `__inst_Functor_fmap_Option` arrives with
   **nb=4**. (Measured by instrumenting that function; both numbers are from
   the same build.)
3. With no bindings there is nothing to specialise on, so no `__spec__` clone
   is minted -- which is why NO Result `fmap` is ever specialised, the typed
   path with a concrete `(Result int int)` included.
4. Every Result `fmap` therefore rides the erased carrier, whose payload
   layout differs from the all-`any` monomorph's, per the two structs above.

So the change is at step 1: a partially-applied head has to keep its type
variables rather than collapse to the carrier. That is exactly the ABI change,
and the carrier is forced there deliberately (the surrounding comment gives the
by-value-struct-receiver reason), so it is not a line to flip -- but it is one
place, not a search. Two things were ruled out by measurement and should not be
re-tried: the registry keying (peeling the head, which yields a silent `false`
from `is?`), and the re-dispatch decline at `emit_abi_register_call`'s
`is_vl_wide_mono` carve-out, which the Result call never reaches.

**And the obvious fix is a trap.** The same peel in
`emit_instance_dispatch_recv_type` is NOT the fix, and was measured not to be.
A hole-headed instance never gets a by-value spec: `.fmap` on a Result resolves
to the erased carrier `__inst_Functor_fmap_Result_tyvar`, whose payloads are
`int64_t`, on the TYPED path too -- while a Saffron `any` is a 16-byte
`tur_tagged_t`. A witness minted on the peeled head therefore calls that
carrier and tags its result with the id of the UNRESOLVED result type: a small
TypeKind number (21), which is the PRIMITIVE tag space that function's own
comment warns about. `type-of` then reads `unknown`, and -- the disqualifying
part -- `is?` on the result answers a silent `false` where the interpreter
answers `true`. That is strictly worse than the panic it replaces, so the
decline stays and the honest `no instance of Functor for Result` is kept. Fix
direction: mint a by-value spec for a hole-headed instance at its all-`any`
instantiation and key the row on that. It is an ABI change touching the typed
path (which is on the carrier today, and correct there), not a keying change.
The measurement is recorded in the comment at
`emit_instance_dispatch_recv_type` so the experiment is not repeated.

**~~M3~~. RESOLVED 2026-09-10: the nullary ctor path (`elab_call.c`) builds the all-`any` instantiation in a Saffron file when no enclosing expectation pins it, matching the field widen for saturated ctors. Pinned by `tests/fixtures/saffron-nullary-parametric-ctor-any`. Was: a nullary ctor of a parametric ADT built in Saffron is not at the `any` instantiation.** `(defdata Opt [a] (Just a) (Nothing))`:
`(is? (Nothing) (Opt any))` is false compiled / true interp; `match` on an
`any` holding `(Nothing)` panics `different instantiation of Opt` compiled.
`saffron-match-parametric-adt` never builds a nullary ctor.

**~~M4~~. RESOLVED 2026-09-10: both runtimes refuse by the box's NAME in the same words (`println: no operator for a Vec argument`): the interpreter's dynamic operator layer generalises the Sym rule to every named any-box, and the compiled `__tur_dyn_argname` consults the type registry. Pinned by `tests/fixtures/saffron-container-in-any-refuses-operator`. Was: `println` of a container through `any`: compiled panics, interp prints a raw pointer.** Vec and Map: compiled panics
`println: no operator for a value of that type`, interp prints a raw
pointer. Option/None/fn/struct/Cons/nil: both panic. For a dynamic dialect,
a printable vector is expected; at minimum the interp pointer print is wrong.

**~~M5~~. RESOLVED 2026-09-10 (parity): `=` on two any-held containers panics `=: no operator for a Vec argument` on both back ends instead of the interpreter answering `false`. Structural equality through `any` remains unsupported by design (the closed set). Was: `=` on containers through `any`: compiled panics, interp answers false.** `(= [1 2] [1 2])`, maps, sets:
compiled panics, interp answers `false` for equal values. `(= (some 7.1)
(some 7.1))` panics on both.

**~~M6~~. RESOLVED 2026-09-10: the dynamic `bit-shr` shifts the unsigned word like the typed builtin and the interpreter. Pinned by `tests/fixtures/saffron-dyn-shift-parity`. The `(bit-shl 1 64)` typed divergence (0 vs 1, a UB shift) is untouched. Was: dynamic `bit-shr` on a negative disagrees with the typed path.**
`(bit-shr -8 1)`: typed compiled 9223372036854775804, interp
9223372036854775804, but through `any` compiled gives -4 (signed shift).
Also `(bit-shl 1 64)` typed: compiled 0, interp 1 with a UBSan report at
`src/turi/eval.c:3924`.

**M7. Dynamic field read on a GENERIC ADT was rejected compiled.** *Update
2026-09-10: the hard error is fixed; the cons-list WALK is not, for a second
reason this entry had folded into the first.* `(defn tl [l] (.tail l))` ->
`no type in this program has a field '.tail'`; interp answered `Cons`.

Root cause: `emit_dyn_field`'s candidate scan skipped any ADT with type
parameters -- a generic ADT has one monomorph per instantiation and their
field OFFSETS differ -- and, separately, any `:heap` ADT, whose C name is
already the pointer type. `Cons` is `(defstruct Cons :heap [A] (head A) (tail
:int))`, so it was excluded twice, and so was every user generic struct. The
scan now names the all-`any` monomorph (the same instantiation a dispatch row
is keyed on, and the only one a Saffron file builds) and lets the TAG COMPARE
discriminate: a `(Duo int cstr)` box carries a different id, matches no arm,
and falls through to `__tur_dyn_no_field`, so no read is emitted at a guessed
offset. `:heap` joins by-value because both box a POINTER to the record; a
single-ctor ADT that is neither is carrier-represented and stays out. An
already-`any` field is also no longer re-widened -- `dyn_widen_to_any` would
cast that 16-byte tagged value through `(int64_t)`, a truncation. `.head` on a
cons list and both fields of a user `(defstruct Duo [A B] ...)` now agree on
both back ends. Pinned by `tests/fixtures/saffron-dyn-field-on-generic-adt`.

**Still open: walking a cons list through an `any`.** `.tail` now READS, but it
reads back an `int`: the field is declared `:int`, a type-ERASED carrier
standing for the recursive `(Cons A)` occurrence, so the widen boxes it with
the int tag and a `cast` to `(Cons any)` panics. The interpreter answers `Cons`
because it inspects the value. That is a distinct defect from the one above --
an erased self-referential field, not a missing candidate -- and it is what
"a cons list cannot be walked through an `any` parameter" now means. Fix
direction: the type-erased tail needs its widen to carry the RECURSIVE
occurrence's tag rather than `:int`'s, which means the erased field must record
what it was erased FROM; hardcoding "a field named tail on Cons is a Cons"
is not it. Note the stdlib idiom already sidesteps this -- a `defdata` with
`any` in both slots, which is what `tests/fixtures/saffron-higher-order` uses
and why `saffron-cons-list` ascribes each step by hand.

**~~M8~~. RESOLVED (verified 2026-09-10): `match` on an `any` holding a stdlib
Option/Result agrees with a user `defdata`, on both back ends.** The arm-pattern
inference in `elab_structs.c` patches an unannotated (`any`) scrutinee to the
`AdtDef` that a constructor pattern names, and nothing on that path was ever
specific to a user `defdata`; the H-series work of the same day (H2's nil-box
miss, H10's lambda-return pin, M10's macro-span seam) is what let the stdlib
sums reach it. Verified across four different `any` SOURCES, since only the
first is a plain parameter default -- an unannotated parameter, a `map-get`
result (a stdlib MACRO, whose span is not the Saffron file), a `vec-get`
element read, and both sum families -- all agreeing compiled and interpreted.
Pinned by `tests/fixtures/saffron-match-any-stdlib-sum`. Was: a static error
(`scrutinee must be an ADT type, got any`, `elab_structs.c:4261`) while a user
`defdata` worked. The guide's `Functor/Applicative/Monad` reachability claim
being "only via `is?`/`cast`" is now stale for `match`; `.fmap` on an any-held
Result is still M2 compiled.

**~~M10~~. RESOLVED 2026-09-10: the seam and the truthiness rule also consult the call's / form's span and, since a macro expansion hides both (`when` is macros.tur's `if` over map.tur's condition), the dialect of the top-level form being elaborated (`Elab.toplevel_saffron`, set beside `toplevel_stmt` in pass 2). Pinned by `tests/fixtures/saffron-macro-any-seam-and-truthiness`; KNOWN row retired. Was: an `any` produced by a stdlib macro gets no checked seam into a typed parameter.** `(defn s [v : int] : int v)` then `(s (map-get #map{:k 7} :k))`
is a static `TUR-E0001: expected int, got any` reported at
`stdlib/map.tur:573`, while `(s (t (map-get ...)))` through an unannotated
defn works. The seam insertion is gated on the ARGUMENT's span being Saffron
(`elab_call.c:6600`); a macro-expanded argument carries the stdlib span.
Affects every `map-*` accessor since they are macros. The `if` truthiness
rule (`elab_forms.c:2862`) is gated the same way: `(if (map-get m k) ...)` is
`if condition must be bool, got any` even in a Saffron file.

**M9. Multi-arg method on `any`: compiled panics as documented, interp
dispatches.** `(.near? x y)` -> compiled `cannot be dispatched dynamically
yet`, interp `true`. Parity note; the guide documents the panic.

## Low -- expressiveness holes, both back ends agree

- ~~`(= "ab" "ab")` through `any` panics `=: no operator for a cstr
  argument`.~~ **RESOLVED 2026-09-10.** `=` / `not=` on two cstrs now answers
  `Eq[cstr]`'s comparison (`stdlib/typeclass-eq.tur`) on both back ends --
  `cstr-eq?`'s documented rule, byte for byte: both-NULL equal, a NULL never
  equal to a non-NULL including the empty string, otherwise strcmp. This is
  the same move H5 made for Sym, and for the same reason: the `=` OPERATOR has
  no cstr row ANYWHERE -- typed Turmeric included, where `(= "ab" "ab")` is a
  TUR-E0006 -- so adding one to the builtin table would change the typed
  surface, while a dialect in which every value is `any` has no other spelling
  for string equality. Two arms, because the two back ends refused at
  DIFFERENT sites: compiled, `__tur_dyn_cmp` fell through to its numeric
  guard; interpreted, a cstr is a SCALAR any-box carrying no struct name, so it
  slipped past the named-box refusal and died on the failed `builtin_lookup`.
  Ordering and mixed-type compares still refuse in the same words on both back
  ends. Pinned by `tests/fixtures/saffron-dyn-string-equality`, which asserts
  both halves.
- `(defn pick [c] (if c 1 "one"))` is a static error (`then=int else=cstr`);
  the if-join only widens under an explicit `: any` (P6).
- ~~`while` rejects an `any` condition (`elab_forms.c:3656`) while
  `if/when/and/or` apply truthiness.~~ **RESOLVED 2026-09-10.** `while` is
  simply the THIRD bool slot a Saffron `any` can reach, after the `if`/`when`
  condition and the `#refine{...}` contract predicate, and
  `elab_saffron_truthy` was already SHARED for exactly that reason -- it just
  had no call in `elab_while`. It returns its argument unchanged for a
  non-`any` (or a non-Saffron file), so the wrap is unconditional at the call
  site and typed `while` is untouched. Pinned by
  `tests/fixtures/saffron-while-any-condition` across three conditions: an
  `any` holding a bool, a `map-get` miss (nil, FALSY -- and a stdlib macro, so
  it also exercises the M10 span seam), and the integer `0`, which is TRUTHY.
  That last one pins the rule rather than the wrap: a C-truthiness `while`
  would exit immediately and print `0` where both back ends print `1`.

- **NEW (found 2026-09-10 while pinning the above): `=` on two Syms is a
  static error unless it goes through an `any`.** `(if (= k :a) ...)` with `k`
  a Sym-typed binding is `TUR-E0006: operator lookup failed for '=', got 2
  arg(s), first arg type Sym` on both back ends, while the same comparison on
  two `any`-held Syms answers pointer identity (H5). So a Saffron file can
  compare keywords only for as long as the type checker does NOT know they are
  keywords -- widening through an unannotated defn makes it start working,
  which is exactly backwards. Same root as the cstr low resolved above (the
  `=` operator has no Sym row, only `Eq[Sym]` does), but it is NOT fixed by
  it: the cstr fix lives in the two DYNAMIC paths, and this shape never
  reaches them. Fix direction: either an `Eq`-backed row for `=` on Sym in the
  builtin table (which changes the typed surface, the thing the cstr fix
  deliberately avoided), or route a failed operator lookup to the class
  instance before reporting TUR-E0006 -- the second is the general answer and
  would subsume the cstr arms too.
- Only a bare symbol can be a dynamic call head: `((adder 1) 1)` and
  `((mkc) 7)` are `expression in call head has type any, which is not
  callable` (`elab_call.c:1796`).
- `(defn f [a & rest : any] ...)` rejects `(f 1 2 3)`: `rest arg 0 has wrong
  type (expected any, got int)` (`elab_call.c:5650`). **Fix direction
  CORRECTED 2026-09-10: "rest args are not widened" is not the bug, and
  widening them would make things WORSE.** Behind that check the
  representation does not exist: passing rest args that are ALREADY `any` --
  `(f 1 (:: 2 any) (:: 7.25 any))` -- does not compile either, with
  `aggregate value used where an integer was expected` on
  `__tur_cons_of((int64_t)(intptr_t)(TUR_TAG(...)))`. A variadic rest list is
  a cons of `__tur_cons_cell { int64_t head; int64_t tail; }` and an `any` is
  a two-word `tur_tagged_t`, so it does not fit the head slot. Relaxing the
  type check alone would trade a clean static error for an uncompilable TU.
  Real fix direction: give the rest list a head that can carry an `any`. The
  machinery already exists one layer over -- a Saffron `(list 1 "two" 7.1)`
  builds a heterogeneous list on the `(Cons any)` monomorph, whose `head` IS
  16 bytes -- so the work is routing the variadic rest lowering onto that
  rather than the older int64 cell; boxing the tagged value behind a pointer
  in the existing cell is the other option, and costs a deref in every rest
  walk. Same representation-gap family as H7/M1/M2-compiled: the THIRD filed
  direction in this report to name a check when the defect is a width.
- ~~`(defstruct Dyn [v : any])` is `unsupported field form` (typed too); ADT
  fields accept `any`, struct fields do not.~~ **RESOLVED 2026-09-10.**
  `defstruct_field_type_lowerable` had no arm for `TY_ANY`, so the gate fell to
  its `default: return false` and a struct with a dynamic field was the one
  field shape that could not be spelled -- while the record `defdata` a
  defstruct LOWERS TO had accepted an `any` field all along. One `case TY_ANY:
  return true`. Pinned by `tests/fixtures/defstruct-any-field`, which is plain
  Turmeric (the hole was never Saffron-specific) and interleaves two `any`
  fields with an int, a cstr and a float: an `any` is 16 bytes among 8-byte
  scalars, so a lowering that got offsets wrong reads back shifted, which a
  struct of one `any` field could never show.

- **NEW, found by the above and fixed with it: a dynamic read of a field that
  is ITSELF `any` emitted a TU cc could not compile.** Four
  `conversion to non-scalar type requested` errors, on
  `return __inst_Hash_hash_T((tur_tagged_t)__r);` and the three `MapKey[T]`
  shims. H11 taught the ABI scan to note every candidate field type of a
  dynamic field read as a widened tag, but an already-`any` field is NOT a
  widen site -- the read hands its tagged value straight back rather than
  boxing it -- so noting it registered `any` ITSELF as a dispatchable tag,
  minting the type-VARIABLE instances at `T = any` whose receiver conversion is
  a scalar-to-struct cast. The scan now skips `TY_ANY` beside `TY_TYVAR` and
  `TY_UNKNOWN`, which is the same rule the emit side already needed (M7's
  "an already-`any` field needs no widen"). Pre-existing and independent of
  the defstruct gate, MEASURED not assumed: a record `defdata` with an `any`
  field and no `defstruct` anywhere produced those four errors before the
  guard. Pinned by `tests/fixtures/saffron-dyn-read-of-any-field`.
- ~~`(map-assoc #map{:a 1} :c 7.1)` fails with `map-assoc-eq-o arg 4: expected
  tyvar, got float`; the value needs an explicit `(:: 7.1 any)` and the
  message does not say so.~~ **RESOLVED 2026-09-10** -- by removing the need
  for the ascription rather than by improving the message. This is the WIDEN
  direction of the D5 seam: the seam handles an `any` ARGUMENT meeting a
  CONCRETE parameter, while here the argument is concrete and the parameter is
  the bare tyvar `V` that nothing had bound, so a Saffron `map-assoc` could
  only ever take int values though it assocs into a `(Map Sym any)`. (The
  error surfaced inside stdlib/map.tur because every `map-*` accessor is a
  macro -- M10's observation.) The fix does NOT widen every concrete argument
  at a tyvar parameter: it binds the tyvar from the SIBLINGS first, exactly as
  H9 does, and widens only when they determine `any`. So the rule keys on what
  the CONTAINER is, not on the dialect -- a `(Map Sym int)` annotated in a
  Saffron file still rejects a float, which
  `tests/fixtures/errors/saffron-map-assoc-typed-value-still-checked` pins
  against the widen relaxing into "concrete at a tyvar always widens here",
  which would erase the element type of every annotated container in the
  dialect. The positive half is
  `tests/fixtures/saffron-map-assoc-widens-value` (a float and a cstr value,
  plus a read of the pre-existing entry). Call-position twin of
  `dl_saffron_widen_elem`, which widens a data literal's elements before the
  homogeneous `vec-of` macro sees them.
- `(defn mkv [] [1 2.5])` is parsed as the generic form (type params `[]`,
  params `[1 2.5]`): `parameter must be a symbol or type annotation`.
- Guide: the boundary example `(scale my-vec 2)` with `v : (Vec int)` panics
  `different instantiation of Vec` when `my-vec` is a Saffron literal
  (always `(Vec any)`); the example only works for a vec built in typed code.
- `(Wrap 7)` passed straight to a `[v : (W any)]` parameter builds `(W int)`
  and is a static `TUR-E0001`, while the same call through an unannotated
  defn builds `(W any)` and passes. The Saffron ctor widen keys on the
  call's position, not the file.
- A `call/cc` receiver annotated `: any` -- `(call/cc (fn [k] : any (k 42)))`
  -- is `conversion to non-scalar type requested` at cc: the CPS call/cc
  emitter assigns its result through a C cast to the binder's type and an
  escape delivers an int64, neither of which is a tagged box. Pre-existing;
  the H10 lambda default deliberately exempts the immediate receiver
  (`in_callcc_receiver`) so an unannotated one keeps its scalar return.
- ~~`Sym` is not accepted as a `defstruct` / `defdata` field type (`defdata:
  field has unrecognized type :Sym`), typed Turmeric too. Found by the fuzzer
  once `sym` joined its scalar pool.~~ **RESOLVED 2026-09-10.** Two rows, one
  per layer: `parse_struct_field_type` had no `Sym` name -> `TY_SYM` mapping
  (so the name fell through to the user-type lookup, which does not know it
  either), and `adt_field_scalar_c_type` had no `TY_SYM` -> `const struct
  __tur_sym *` row. A Sym is an interned record POINTER -- a pointer-sized
  scalar carrier like the `cstr` beside it -- so neither layer needed a new
  representation. The second row matters even though the first alone RUNS
  correctly: without it the field took the `int64_t` default and the ctor was
  emitted taking `int64_t` while its caller passed the pointer, a right answer
  with a `-Wint-conversion` under it. Pinned by
  `tests/fixtures/defstruct-sym-field`, which compiles warning-free and puts a
  `float` field after the Sym so a layout shift would show.

  **A THIRD row was needed, found by re-running the fuzzer's probe:** the field
  type alone did not make that probe pass, because a record whose first field
  is `Sym` is CONSTRUCTED with a keyword in that position --
  `(make-struct P :kw)` -- and a leading keyword was always read as a FIELD
  NAME (`keyword construction needs :field value pairs`), so such a record
  could not be built at all. `(Circle :diameter 2.0)` (a typo) and
  `(make-struct Q :label 9.75)` (a Sym value then a float) are the same shape
  syntactically, so no argument-count rule separates them -- the first attempt
  used odd/even and broke three `errors/` fixtures that pin the precise
  unknown-field diagnostic. The declared TYPES do separate them: positional is
  chosen only when the keyword names no field, the argument count equals the
  field count, AND the first field is `Sym`-typed, so `Circle` (one field, two
  arguments) keeps its `unknown field 'diameter'`. Pinned by
  `tests/fixtures/ctor-keyword-vs-sym-value`, which carries all four shapes in
  one file because the rule is a choice BETWEEN them. Both this and M7 had
  their `KNOWN` rows retired from `tests/saffron-fuzz-src.py`, and 250 fuzz
  cases with those shapes back in the default pool are clean.
- `(fn [] :kw)` is `fn: missing body`: a keyword literal in body position is
  read as a return annotation. `(fn [] (:: :kw Sym))` works. Found by the
  fuzzer; the generator avoids both shapes (KNOWN rows) and pins them with
  `--known-probes`.
- ~~Cosmetic: `vec-get` out of bounds reads `tvec index out of bounds`
  compiled vs `vec index out of bounds` interp.~~ **RESOLVED 2026-09-10.**
  `tvec` was a leftover internal name -- the type is `Vec` -- so the compiled
  side took the interpreter's spelling at both `stdlib/vec.tur` sites
  (`vec-get` and `vec-data-get-checked__`). Snapshot regen in the same change,
  as the fixture rule requires: 148 `expected.c` files, 2 lines each, and the
  whole diff is that one string -- no codegen drift.

## Stale text to fix alongside

- `docs/guides/saffron-guide.md:134-136` (the "not dispatchable yet" sentence).
- `src/compiler/elab_toplevel.c:618-621` and the header comment of
  `tests/fixtures/saffron-map-literal` both say `Hash[any]`/`MapKey[any]`
  "do not exist".

## Method

Probe corpus: ~60 small programs, each run compiled and interpreted, output
diffed, non-zero exits and `Segmentation fault`/`Abort` flagged. Every float
probe used a non-zero fraction (7.1, 7.25, 3.55). The productive axes were:
the kind held in the box (Sym, Nothing-ctor, closure, typed defn, container)
x the route it took (forward ref, capture, seam into typed param, generic
stdlib fn with a `(& K)` param, dynamic method witness) x the operation
(the eight dynamic ops, container get/assoc/miss, `is?`/`cast`/`match`,
`println`, `=`).
