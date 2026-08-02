# `set!` on a by-value struct parameter: a silent no-op compiled, a write interpreted

**Severity: medium-high.** Two defects, and the second is the one that bites:

1. **The same program prints different answers** under `tur run` and
   `tur --interpret`, with no warning from either.
2. **The compiled backend silently discards the write.** `(set! (.n a) 3)` on a
   by-value struct parameter emits a store into a stack temporary that is
   immediately dropped; `cc -O2` deletes it outright. An author who writes a
   mutation gets *nothing*, and no diagnostic says so.

(1) is what a test harness notices. (2) is what a user hits. Both have the same
root cause, and fixing (2) resolves (1) as a side effect.

Found 2026-08-02 while landing `#writes` frames
(`docs/upcoming/checked-write-frames-plan.md`); unrelated to that work, which
is only how it surfaced.

## Repro

```turmeric
(defstruct Ctr [n : int])

(defn set-it! [^mut a : Ctr] : int
  (set! (.n a) 3)
  0)

(defn peek [^borrow a : Ctr] : int
  (.n a))

(defn main [] : int
  (let [^mut c (Ctr 0)]
    (set-it! c)
    (println (peek c)))
  0)
```

```
$ tur run f.tur           # compiled
0
$ tur --interpret f.tur    # turi tree-walking interpreter
3
```

**Flag-position trap:** `tur run --interpret f.tur` prints `0` -- it does NOT
take the turi path. Only `tur --interpret <file>` (global flag, before the
file, which is what `tests/run-turi.sh` uses) reaches `src/turi/eval.c`. This
cost a wrong "cannot reproduce" during the investigation.

## Root cause, both sides

**Compiled -- a dead store.** `defstruct` lowers to a by-value C struct, passed
by value:

```c
typedef struct tur_adt_Ctr { int64_t n; } tur_adt_Ctr;

static int64_t set_hyit_ex(tur_adt_Ctr a) {
        (a).n = INT64_C(3);      /* writes the parameter copy, then discards it */
        return INT64_C(0);
}
```

Compiling that C at `-O2` yields, in full:

```
_set_it:
	mov	x0, #0
	ret
```

The store is gone. This is not "the write is unobservable"; the write does not
happen.

**Interpreted -- a shared heap object.** `TuriStruct` is heap-allocated once
(`src/turi/eval.c:570`), `TuriValue` holds only the pointer
(`src/turi/value.h:55`), and parameter binding copies the `TuriValue` -- i.e.
the pointer -- without cloning:

```c
    EvalFrame *call_frame = eval_frame_new(env, (EvalFrame *)cl->captured);
    for (uint32_t i = 0; i < n_args; i++)
        frame_bind(env, call_frame, fn->params[param_offset + i]->name->name, args[i]);
```
(`src/turi/eval.c:7153`; two more identical sites at `:5726` and `:6783`. None
consults `^mut`/`^borrow`.)

`EX_SET_FIELD` then writes through that shared pointer
(`src/turi/eval.c:8020-8044`, `s->fields[idx] = v;`).

## Which spelling actually works

**`rc<Struct>` does, and it agrees across both backends** -- verified:

```turmeric
(defn set-rc! [a : rc<Ctr>] : int (set! (.n a) 3) 0)
(defn main [] : int
  (let [c (rc/of (Ctr 0))]
    (set-rc! (rc/clone c))     ;; rc is unique; clone to share
    (println (.n c)))
  0)
```
```
$ tur run f.tur          -> 3
$ tur --interpret f.tur  -> 3
```

`:heap` structs are the other shared-mutation path
(`docs/archive/vec-typed-pointer-vertical-slice-plan.md:234`: "a mutation in a
callee is visible to the caller").

And a `&Struct` parameter is already **rejected**:

```
error: set! (.field s): receiver must be a struct or rc<Struct>, got &<adt>
```

So the language already has a correct answer and already rejects one wrong
spelling. The gap is that the *other* wrong spelling -- a plain by-value struct
parameter -- is accepted and silently does nothing.

## Blast radius: smaller than expected

- **No fixture depends on either semantics.** A scan of every
  `tests/fixtures/*/input.tur` for `(set! (.` inside a `defn` with an annotated
  parameter found exactly one case, `wf1-writes-frame-honored`, which was
  rewritten on 2026-08-02 specifically to stop observing the write. The suite
  has committed to *neither* model.
- **One fixture has the latent bug.** `catch-box-user-sink-confines` declares
  `(defn stash [s : cstr ^mut slot : Slot] : nil (set! (.held slot) s))` and
  calls it to "stash" a panic message. The stash does not happen. The fixture
  passes because it never reads `slot` back -- so this is a real instance of
  the footgun sitting in-tree, doing nothing, unnoticed.
- The WF2 negatives (`errors/wf2-writes-exceeds-{set,mut-pass}`) use the
  pattern deliberately, as *syntactic* frame violations. They would need
  rewriting to an `rc<Struct>` receiver if the pattern becomes an error.

## Documentation gaps found on the way

These are arguably the reason the divergence survived this long:

- **`^mut` is not defined in any guide.** No `structs-guide.md`,
  `syntax-guide.md`, `type-annotations-guide.md`, or `style-guide.md` section
  says what it means on a parameter.
- **The one guide passage that discusses it says the wrong thing.**
  `docs/guides/uniqueness-types-guide.md:116-127` calls `^unique ^mut` "unique
  mutable *references*", "the ownership-transfer equivalent of `&mut T`", and
  says such a parameter "may mutate the value in place". Read plainly, that
  promises exactly the by-reference behavior the compiled backend does not
  provide. The by-value model is stated only in
  `docs/upcoming/v1/refine-stateful-measures-plan.md:182`.
- **`docs/guides/turi-parity-guide.md:57`** lists `Structs / ADTs | OK | OK`.
  That parity row is wrong for mutation through a parameter.
- **`structs-guide.md` never states struct value semantics at function
  boundaries** at all.

## Fix directions

Ranked, and the first two are complementary rather than alternatives:

1. **Diagnose the no-op write.** Reject (or warn on) a place-expression `set!`
   whose receiver is a by-value struct *parameter*, pointing at `rc<Struct>` /
   `:heap`. This kills the footgun, and it kills the divergence class as a side
   effect -- the diverging programs stop compiling. The `&Struct` receiver is
   already rejected with a message of exactly this shape, so it is a
   consistency fix as much as a new check. Blast radius: two of my own WF
   fixtures plus `catch-box-user-sink-confines` (which the change would be
   *revealing a bug in*, not breaking).
2. **Make turi copy structs on parameter bind**, so the interpreter matches the
   documented model. One deep-copy of `TURI_STRUCT` args at the three
   `frame_bind` loops, or a wrapper. Zero fixture blast radius. Do this even if
   (1) lands, so the two backends agree on programs that still compile.
3. **Fix the docs** regardless of (1) and (2): define `^mut` on a parameter,
   correct the `uniqueness-types-guide.md` "mutable references" passage,
   correct the turi-parity row, and state struct value semantics in
   `structs-guide.md`.
4. **Cheap, non-exclusive:** teach `run-turi.sh` to name a
   compiled-vs-interpreted semantic divergence instead of reporting a bare
   `stdout mismatch`. The shape is distinctive enough to detect.

## Not affected

The refinement analyses are conservative in the direction that survives either
model: WF2 counts a place write rooted at a parameter as a write regardless of
observability, and WF3's rescue requires a plain-symbol target that is never
borrowed. A by-reference interpreter makes those rules pessimistic, not
unsound. (The WF2 header comment in `elab_fns.c` overstated this as "does reach
the caller's object"; corrected 2026-08-02 to say over-approximation.)
