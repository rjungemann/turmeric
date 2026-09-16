# Session message payloads are int64-only: `float` silently truncates, pointers and structs fail to build

**Severity: high.** The session runtime carries every message as a bare
`int64_t`. The type checker accepts `(Send float ...)`, `(Send cstr ...)`,
`(Send SomeStruct ...)` and `(Send (Session P) ...)` without complaint, and all
of them are correct under `tur --interpret` -- but on the compiled path:

| Payload | Compiled result |
| --- | --- |
| `int`, `bool` | correct |
| **`float`** | **silently truncated** -- `7.25` arrives as `7`. No diagnostic, exit 0 |
| `cstr` | **cc error on macOS** (`-Wint-conversion`); works on Linux gcc, which only warns |
| `defstruct` / ADT by value | **cc error on every platform** |
| `(Session P)` / `(Role G R)` -- i.e. **delegation** | **cc error on macOS**, same `-Wint-conversion` |

The float row is the serious one: a silent wrong answer with no diagnostic
anywhere. The others are loud, but they are build failures with a C-level
message that never mentions sessions, on programs the Turmeric type checker
accepted.

This is why every session fixture and every example in
[session-types-guide.md](../guides/session-types-guide.md) sends `int`.

## Repros

All measured against `./build/tur` at v0.48.0, 2026-09-16, Apple clang 21.0.0
(arm64-apple-darwin27). Each uses the fixture-standard pthread `spawn`/`join`
peer (elided here; see any `tests/fixtures/session-*/input.tur`) because an
`async` peer deadlocks the compiled binary for an unrelated reason --
[compiled-async-fiber-deadlocks-on-a-session-op](compiled-async-fiber-deadlocks-on-a-session-op.md).

### float -- silent wrong answer

```turmeric
(defn main [] : int
  (let [[s r] (make-session (Send float Close))]
    (let [t (spawn (fn [] (let [[v r] (recv r)] (println v) (close r))))]
      (let [s (send s 7.25)] (close s) (join t))))
  0)
```

```
$ ./build/tur build c-float.tur -o c-float.bin    # exits 0, no diagnostic
$ ./c-float.bin
7                       <-- WRONG; 7.25 was sent

$ ./build/tur interpret c-float.tur
7.25                    <-- correct
```

(Per the float-testing rule in [CLAUDE.md](../../CLAUDE.md) the probe literal has
a non-zero fractional part; `7.0` would have hidden this entirely.)

### cstr -- builds on Linux, not on macOS

```
$ ./build/tur build c-cstr.tur -o c-cstr.bin
...c-cstr_tur.c:5363:26: error: incompatible integer to pointer conversion
  initializing 'const char *' with an expression of type 'int64_t' [-Wint-conversion]
1 error generated.
tur: cc invocation failed (status 256)
```

`-Wint-conversion` is a hard error on Apple clang 21 and only a warning on Linux
gcc (the platform split recorded for the three AppleClang-21 strictness
promotions). Compiling the same emitted C with the warning downgraded shows the
value round-trips correctly:

```
$ ./build/tur emit-c c-cstr.tur > c-cstr.c
$ cc -Wno-int-conversion -o c-cstr-lax.bin c-cstr.c -lpthread -lm && ./c-cstr-lax.bin
hello                   <-- correct
```

So the cstr case is **not** a wrong answer -- a pointer survives the int64 round
trip intact. It is purely a C type-checking failure, which is why it is
platform-dependent. Treat "works on Linux" as accidental, not as the intended
contract.

### defstruct -- cc error everywhere

```
$ ./build/tur build c-struct.tur -o c-struct.bin
...error: initializing 'tur_adt_Pt' with an expression of incompatible type 'int64_t'
...error: operand of type 'tur_adt_Pt' where arithmetic or pointer type is required
2 errors generated.
```

No int/pointer conversion exists for a by-value struct, so no compiler accepts
this one.

### Delegation -- the same failure, on a headline feature

Sending a session endpoint over a session is standard session-type delegation
and is discussed in the guide. It has no fixture, and it does not build on macOS:

```turmeric
(let [[si ri] (make-session (Send int Close))]
  (let [[so ro] (make-session (Send (Session (Recv int Close)) Close))]
    ...  (let [so (send so ri)] ...)))
```

```
...error: incompatible integer to pointer conversion initializing 'void *'
  with an expression of type 'int64_t' [-Wint-conversion]
```

Correct under `--interpret` (prints `42`).

## Root cause

One signature, in the C preamble `emit_module.c` emits:

```c
static void    tur_session_send(TurChannel *ch, int64_t val);
static int64_t tur_session_recv(TurChannel *ch);
```

and one call-site template in `src/compiler/elab_sessions.c:306`:

```c
"({ tur_session_send(__TUR_VAL_0__, (int64_t)(__TUR_VAL_1__)); (void *)__TUR_VAL_0__; })"
```

The payload is coerced to `int64_t` by a plain C cast, and the receiving binding
is declared at the protocol's payload type, so the emitted code is:

```c
void * s_1620 = ({ tur_session_send(__t286, (int64_t)(__t287)); (void *)__t286; });
double v_1612 = tur_session_recv(__t128);
```

For a `double` operand `(int64_t)(...)` is a **value conversion that truncates**,
not a reinterpretation -- hence `7.25 -> 7 -> 7.0`. For a pointer it is
bit-preserving but ill-typed in C. For a struct no conversion exists at all.

The interpreter is unaffected because `TuriValue` is a tagged union that carries
the payload at its own type, which is why all four cases are correct under
`--interpret`. Another instance of the inverted parity the session audit found
throughout.

## Fix directions

1. **Bit-reinterpret rather than value-convert** at the two seams. For `float`
   the fix is a `memcpy`/union pun into the int64 slot on send and back out on
   recv, the same treatment sized primitives already get ("carrier ascription
   bit-reinterprets correctly" -- [turi-parity-guide.md](../guides/turi-parity-guide.md)).
   That closes the silent-wrong-answer row, which is the one worth closing first.
2. **Cast pointers explicitly** (`(int64_t)(intptr_t)` on send, `(T)(intptr_t)`
   on recv) so `cstr`, `Session` and `Role` payloads stop depending on how strict
   the host C compiler is. Cheap, and it un-breaks delegation on macOS.
3. **Structs by value** need a real decision: box and send the pointer, or
   reject them at elaboration with a session-level diagnostic. Either is better
   than a cc error naming `tur_adt_Pt`.
4. **Whatever is not supported must be rejected in the elaborator**, with
   `TUR-E0212` naming the payload type -- not passed through to cc. The type
   checker currently accepts all four.

Direction 4 is the floor: if 1-3 are deferred, a user should still learn from
`tur build` that a `float` payload is not supported, rather than from a wrong
number at run time.

## Tests to add

- `tests/fixtures/session-payload-float/` -- send `7.25`, expect `7.25`. Fails
  today.
- `tests/fixtures/session-payload-cstr/` and `.../session-payload-struct/`.
- `tests/fixtures/session-delegation-over-channel/` -- the delegation repro,
  which has no fixture on either path today.

Each also wants a `-turi` twin, or one shared source once
[compiled-async-fiber-deadlocks-on-a-session-op](compiled-async-fiber-deadlocks-on-a-session-op.md)
lands a portable peer spawn.

## See also

- `src/compiler/elab_sessions.c:306` -- the send template's `(int64_t)` cast.
- `emit_module.c` -- the `tur_session_send`/`recv` int64 signatures.
- [session-types-guide.md](../guides/session-types-guide.md) -- documents no
  payload restriction; every example sends `int`.
- [turi-session-expansion-plan.md](../upcoming/turi-session-expansion-plan.md)
  -- the audit this came out of.
