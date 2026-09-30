# tests/fuzz -- libFuzzer targets for the parsers that take untrusted bytes

These targets feed the parsers that take untrusted bytes, the ones
[the security guide](../../docs/guides/security-guide.md) promises to handle
safely, through libFuzzer under ASan and UBSan. They came out of
[security-audit-plan](../../docs/upcoming/security-audit-plan.md) WP4.

The existing `tests/*-fuzz-src.py` harnesses are different. They generate
whole Turmeric programs and check that the compiler handles them. These
targets are in-process and coverage-guided, and each one exercises a single
parser.

| Target | Parser | Boundary |
| --- | --- | --- |
| `fuzz_serial_cont` | `tur_serial_cont_check` / `tur_serial_cont_deserialize`, the emitted serial-continuation runtime (`bytes->serial-cont`, `resume-cont!`, `image/blob-resume!`) | T2 |
| `fuzz_serial_wire` | `serial_cont_from_bytes`, the TSER wire codec (`src/runtime/serial.c`) | T2 |
| `fuzz_image_header` | `tur_image_read_header` + `tur_image_verify_payload` (`tur image-info` / `image-verify`) | T2 |
| `fuzz_json_compiled` | `json/decode` / `json/encode` as compiled programs ship them (`stdlib/json.tur`) | T2 |
| `fuzz_json_interp` | the interpreter's `json/decode` twin (`src/turi/interpreter_natives.c`) | T2 |
| `fuzz_httpd_head` | `httpd-parse-head` plus the header, cookie, form, multipart and JSON accessors (`stdlib/httpd.tur`) | T2 |
| `fuzz_lsp_frame` | `Content-Length` framing for `tur lsp` / `tur dap` (`src/lsp/lsp_io.c`) | T5 |
| `fuzz_reader` | the reader, all six dialects (`src/compiler/reader.c`) | T1 |
| `fuzz_manifest` | `pkg_manifest_read`, plain and sweet-exp `build.tur` | T1 |
| `fuzz_justfile` | `parse_justfile` (`src/compiler/justrun.c`) | T1 |

Some of these parsers only exist as stdlib inline C: compiled JSON, the serial
runtime and httpd. Those three targets do not fuzz a copy of that code. At
build time, the build runs `tur emit-c` over a small entry program
(`*_entry.tur`) and the harness `#include`s the result, so it drives exactly
the code a compiled program ships.

`fuzz_justfile` cannot run a command. Today the Justfile parser evaluates
backticks while it parses (plan finding D-2). The harness therefore renames
`popen`, `system`, `setenv` and `chdir` to inert stand-ins before it includes
`justrun.c`, which means a made-up backtick fails the way an unrunnable one
does.

## Build

This needs clang with compiler-rt, since libFuzzer ships there. On Ubuntu that
is `libclang-rt-18-dev`. Use a separate build directory, because `TUR_FUZZ`
instruments all of `tur_core` for coverage.

```sh
cmake -S . -B build-fuzz -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_COMPILER=clang -DTUR_FUZZ=ON
cmake --build build-fuzz -j --target tur_fuzz_all
```

The binaries land in `build-fuzz/fuzz/`. Each target is also a ctest that
replays its committed seeds once:

```sh
ctest --test-dir build-fuzz -R '^fuzz_'
```

## Run

```sh
# every target, 60 s each, nproc at a time; findings under fuzz-findings/
bash tests/fuzz/run-fuzzers.sh build-fuzz 60 fuzz-findings

# only some targets
TUR_FUZZ_TARGETS="fuzz_reader fuzz_manifest" bash tests/fuzz/run-fuzzers.sh build-fuzz 600

# one target, by hand
build-fuzz/fuzz/fuzz_reader tests/fuzz/seeds/fuzz_reader -max_total_time=300
```

`run-fuzzers.sh` starts each target from its seeds in `seeds/<target>/`. It
adds inputs the tree already has:

- every fixture and stdlib `.tur` file, for the reader
- every `build.tur`, for the manifest reader
- every `.json` file, for both JSON decoders
- the repo `Justfile`

The script exits 1 if any target stops on a finding, and names those targets in
`<findings-dir>/FAILED`.

The nightly `fuzz-parsers` job in `.github/workflows/fuzz.yml` runs every
target for 10 minutes. When a target finds something, the job fails rather
than opening an issue. A crash in one of these parsers may be a
vulnerability, so triage it privately first ([SECURITY.md](../../SECURITY.md)).

## Two input conventions

- **`fuzz_reader`**: the first byte picks the reader (`'0'`..`'5'`: s-expr,
  curly-infix, neoteric, sweet, r7rs, r7rs-sweet, taken modulo 6). The rest
  of the input is the source.
- **`fuzz_manifest`**: an even first byte (`p`) writes the rest to `build.tur`.
  An odd one (`s`) writes it to `build.tur.sweet`.

Some targets repair checksums so that the fuzzer can get past them:

- `fuzz_serial_wire` appends the TSER CRC.
- `fuzz_image_header` makes a second pass with a valid magic and a
  recomputed header CRC.

## When a target finds something

1. Reproduce: `build-fuzz/fuzz/<target> <crash-file>`.
2. Fix the parser.
3. Add the reproducer to `seeds/<target>/`, so that the ctest replay and every
   nightly run cover it from then on.
4. If the input also makes a readable regression test, add a fixture under
   `tests/fixtures/` that exercises the same input through the public API.
