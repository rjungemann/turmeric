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

**H7. Seam into a fn-typed parameter emits uncompilable C.** *Update 2026-09-10:* representation gap, not a gate. The seam's checked unbox spells the payload `(void *)` while a `(fn [int] int)` parameter is emitted as the three-word `tur_poly_fn_t` (env, fn, fn_cps), and the widened value's box id is the fat-closure id, not the plain-function id the check wants, so a compiling version would panic "a closure that captures where a plain function is required" -- which is the honest outcome for a capturing closure, and the wrong one for a captureless typed lambda (`static_ok` on its `EX_FN_TO_FAT`), whose code pointer could be recovered. Fix direction: at the compiled `EX_ANY_CAST` for a `TY_FN` target, accept the fat id when the box's env is the static (captureless) one and build the `tur_poly_fn_t` from the fat box; otherwise take the panic path. Left open.

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

**M1. `.bind` on an `any` Option gives the wrong answer compiled.** `(defn
half [o] (.bind o (fn [x] (some (/ x 2.0)))))` -> originally `cast: any holds a
function this cast cannot accept` (the witness casts the lambda to `(fn [any]
any)`; a bind lambda returns `(Option any)`). Interp: 3.55. `.fmap` on Option
works. **Update 2026-09-10** (after H10 pinned lambda returns to `any`): the
cast now passes and `(type-of (half (some 7.1)))` is `unknown` compiled, then
the next `cast` panics `any holds unknown, not Option`.

**Fix direction CORRECTED 2026-09-10 -- the note below replaces "the witness's
`: any` return must not re-widen a body that is already `any`", which the
emitted C does not bear out.** The body is not an `any` being re-boxed. Read
`__dynwit_Monad_bind_Option`: it calls `__inst_Monad_bind_Option`, the erased
CARRIER (`static int64_t __inst_Monad_bind_Option(int64_t, tur_poly_fn_t)`),
and tags its `int64_t` result `TUR_TAG(21, ...)` -- 21 being a bare TypeKind
number, which is why `type-of` reads `unknown`. There is no by-value spec for
`bind` at `(Option any)` the way there is for `fmap`, because `bind`'s
continuation is declared `k : (fn [a] (m b))` (`stdlib/typeclass-monad.tur:13`)
and the witness casts it to `(fn [any] any)`, so the result instantiation never
grounds. Worse than the tag: inside that carrier the Saffron lambda is CALLED
through `((int64_t (*)(void*, int64_t))k.fn)`, while the lambda returns a
16-byte `tur_tagged_t` -- half the box is dropped on the return. So this is a
truncation, not a nesting.

Why the obvious repair does not work either: casting `__a1` to `(fn [any]
(Option any))` -- which the class declaration DOES record enough to do -- would
let H10's "unless an expected fn type decides it" leave the body unboxed. But
the lambda is boxed at the ORIGINAL dynamic site, `(.bind o <lambda>)`, where
`o` is an `any` and `m` is unknown, so the widen has already happened by the
time the witness sees it. `(fn [any] any)` is forced there.

This is the same SHAPE as H7 and M2's compiled half: a Saffron `tur_tagged_t`
meeting a representation the typed path already chose. Whichever of the three
is picked up first should carry the other two.

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

**Still open, compiled -- and the obvious fix is a trap.** The same peel in
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

- `(= "ab" "ab")` through `any` panics `=: no operator for a cstr argument`.
  Bool and numeric `=` work. String equality is table stakes for the dialect.
- `(defn pick [c] (if c 1 "one"))` is a static error (`then=int else=cstr`);
  the if-join only widens under an explicit `: any` (P6).
- `while` rejects an `any` condition (`elab_forms.c:3656`) while
  `if/when/and/or` apply truthiness.
- Only a bare symbol can be a dynamic call head: `((adder 1) 1)` and
  `((mkc) 7)` are `expression in call head has type any, which is not
  callable` (`elab_call.c:1796`).
- `(defn f [a & rest : any] ...)` rejects `(f 1 2 3)`: `rest arg 0 has wrong
  type (expected any, got int)` (`elab_call.c:5650`); rest args are not
  widened.
- `(defstruct Dyn [v : any])` is `unsupported field form` (typed too); ADT
  fields accept `any`, struct fields do not.
- `(map-assoc #map{:a 1} :c 7.1)` fails with `map-assoc-eq-o arg 4: expected
  tyvar, got float`; the value needs an explicit `(:: 7.1 any)` and the
  message does not say so.
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
- `Sym` is not accepted as a `defstruct` / `defdata` field type (`defdata:
  field has unrecognized type :Sym`), typed Turmeric too. Found by the fuzzer
  once `sym` joined its scalar pool.
- `(fn [] :kw)` is `fn: missing body`: a keyword literal in body position is
  read as a return annotation. `(fn [] (:: :kw Sym))` works. Found by the
  fuzzer; the generator avoids both shapes (KNOWN rows) and pins them with
  `--known-probes`.
- Cosmetic: `vec-get` out of bounds reads `tvec index out of bounds`
  compiled vs `vec index out of bounds` interp.

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
