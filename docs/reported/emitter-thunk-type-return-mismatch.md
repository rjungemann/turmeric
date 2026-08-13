# The emitted typed-thunk ABI disagrees with a lifted lambda's return type

**Severity: low-medium (undefined behavior, currently benign on x86-64/arm64).**
Same class and same consequences as
[reactor-fd-callback-fn-ptr-type-mismatch](../archive/reactor-fd-callback-fn-ptr-type-mismatch.md):
the call works because the mismatched returns share an ABI slot, but it is UB
that `-fsanitize=function` reports and that CFI (`-fsanitize=cfi-icall`),
CET/BTI-hardened builds, and WASM's `call_indirect` type check turn into a hard
failure.

**Status:** open. This is the residue of that report's fix direction 2, filed
2026-08-13 after its directions 1 and 3 landed and closed 29 of the 32 findings.
The remaining 3 have a different cause than the report guessed, which is the
point of this filing.

## What is left

With `stdlib/httpd.tur` and `src/async/reactor.c` corrected, a full
clang/UBSan run of the fixture suite reports exactly three sites, all inside
emitted C, all **return-type-only** disagreements:

```
tests_fixtures_httpd-mw-fold-many_input_tur.c:11595:29: runtime error: call to
  function __fn_2295 through pointer to incorrect function type
  'long (*)(void *, long)'
  note: __fn_2295 defined here      -> static void *  __fn_2295(void *, int64_t)

tests_fixtures_httpd-mw-fold-many_input_tur.c:11515:13: runtime error: call to
  function __fn_2304 through pointer to incorrect function type
  'void (*)(void *, void *)'
  note: __fn_2304 defined here      -> static int64_t __fn_2304(void *, void *)

tests_fixtures_httpd-mw-compose-of_input_tur.c:11596:29: __fn_2337, same shape
```

The parameter lists match in every case. Only the return type differs, and it
differs in **both directions**: once `int64_t` expected against a `void *`
definition, once `void` expected against an `int64_t` definition.

## Not the cause the parent report assumed

The parent report attributed these to "the emitter's own closure-invocation
lowering ... a fixed `void (*)(void *, int64_t)` shape". That is not what
happens. `typed_thunk_typedef_name` (`src/compiler/emit_module.c:410`) builds the
typedef name from `type_c_name(result_type)` and each parameter's `type_c_name`,
so the thunk type **is** derived from a real signature. The two signatures
involved are simply not the same one:

- the **call site** names the thunk from the type of the closure *sink* it is
  calling through, and
- the **lambda** is emitted from its own declared type.

Where the sink is type-erased, those diverge. In `httpd-mw-fold-many` the sink
is `httpd-mw-fold`'s `:int` middleware parameter, while the lambda is
`(fn [n : int] : ptr<void>)` -- so the call site says `int64_t` and the
definition says `void *`.

Which makes this a **downstream symptom of the `:int` closure sinks in the
httpd API** (`^fat handler : int`, `mw : int`, `verifier : int`), exactly the
pattern CLAUDE.md's "No Lazy `:int` Stand-Ins" rule names. The parent report
spotted that rule violation on the C side; this is the same violation on the
Turmeric side, and it is what makes the emitter unable to agree with itself.

The `void` vs `int64_t` case is a second, smaller thing worth confirming
separately: `__fn_2304` is declared `: nil` in source but emitted returning
`int64_t`, while the thunk typedef for the same `nil` says `void`. That looks
like a `nil`-return lowering inconsistency between the lifted-lambda emitter and
`type_c_name`, independent of the erasure above.

## Repro

Needs clang -- GCC has no `-fsanitize=function`, so a GCC Debug build reports
nothing here no matter how broken the types are.

```sh
sudo apt-get install -y libclang-rt-18-dev        # else arena.c cannot find
                                                  # sanitizer/asan_interface.h
cmake -S . -B build-clang -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_C_COMPILER=/usr/bin/clang
cmake --build build-clang -j
CC=/usr/bin/clang TUR=./build-clang/tur \
  TUR_TEST_FILTER='^httpd-mw-(fold-many|compose-of)$' bash tests/run.sh
grep -h "incorrect function type" tests/fixtures/httpd-mw-*/actual.stderr
```

The fixtures **PASS** while emitting these -- UBSan defaults to
print-and-continue -- so the summary line is not the signal. Read
`actual.stderr`.

## Fix directions

1. **Type the httpd closure sinks.** `^fat handler : int` becomes
   `(fn [ptr<void>] nil)`, `mw : int` becomes the real middleware arrow, and so
   on. Then the call site and the lambda name the same thunk typedef and the
   mismatch cannot arise. This is the real fix, it is what the `:int` rule asks
   for, and it is an API change across `stdlib/httpd.tur` plus every fixture and
   spice that builds a handler.
2. **Check the `nil`-return lowering** independently: find why a `: nil` lambda
   is emitted returning `int64_t` while `type_c_name` gives the thunk `void`.
   That one may be a small, self-contained emitter fix and is worth confirming
   before attempting 1.
3. **Then add the UBSan gate** the parent report's direction 3 asks for. It
   cannot pass today -- these three findings would fail it -- which is a good
   reason to keep this report open rather than fold it into the parent.

Do 2 first; it is cheap and it tells you whether 1 is the whole story.

## Also worth knowing: clang builds are not currently green

Found while setting up the verification above. Independent of this report, but
anyone following the repro will hit them:

- `src/compiler/elab_memory.c` had no trailing newline, so any clang build fails
  at `-Werror,-Wnewline-eof` before reaching the sanitizer at all. **Fixed** in
  the parent report's landing.
- Three fixtures fail under clang and pass under GCC, on `main`, unrelated to
  any of this: `complex-basics` (times out), `complex-smith-div` (`nan` where
  `1.00499e+200` is expected -- a float-overflow behaviour difference), and
  `load-in-imported-module` (prints `0` for `5`). Verified pre-existing by
  building the parent commit with clang. Not investigated further; the last one
  in particular is not obviously a float issue and may be worth its own look.

## References

- `src/compiler/emit_module.c:410` -- `typed_thunk_typedef_name`
- `src/compiler/emit_module.c:427` -- `ensure_typed_thunk_typedef`
- `stdlib/httpd.tur:667,772,811,2752,2829,3058` -- the `^fat ... : int` sinks
- `tests/fixtures/httpd-mw-fold-many`, `tests/fixtures/httpd-mw-compose-of`
- [docs/archive/reactor-fd-callback-fn-ptr-type-mismatch.md](../archive/reactor-fd-callback-fn-ptr-type-mismatch.md) -- the parent
