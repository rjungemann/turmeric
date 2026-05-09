# Turmeric

A Lisp that compiles to C99. See [turmeric-plan.md](turmeric-plan.md) for design.

## Build

```sh
make           # debug build (sanitizers on)
make release   # optimized
make test      # run fixture tests
make clean
```

The compiler binary lands at `build/tur`. Use:

```sh
./build/tur build path/to/file.tur     # → executable
./build/tur emit-c path/to/file.tur    # → C source on stdout
./build/tur run path/to/file.tur       # build + execute
```

## Status

- **Phase 0** ✅ — minimal end-to-end pipeline; `(println "hi")` round-trips.
- **Phase 1** ✅ — full reader (vectors, keywords, hex/binary ints), typed IR, elaborator, operator dispatch table, codegen for `def`/`let`/`if`/`do`/`when`/`unless`/`cond`/`set!`/`while`, `^mut` bindings. Exit: fizzbuzz 1..100. 12/12 fixtures green, ASan/UBSan clean.
- **Phase 2** — top-level `defn`, `extern-c`, inline-C blocks, multi-file build with generated `_main.c`. Up next.
