# The fat-dispatch shim selector uses the SysV aggregate-return threshold on Win64

**Summary:** `adt_app_byval_pass_by_ptr` decides "register-returned vs sret"
with `size > 16`, which is the **System V** rule. Win64 returns an aggregate in
RAX only when its size is exactly 1, 2, 4, or 8 bytes; everything else goes
through a hidden pointer. So a 16-byte monomorph is sret on Windows while the
selector still classifies it as register-returned, keeps the generic forwarding
shim, and the resulting signature mismatch jumps to a garbage address.

**Severity:** Medium. One fixture, but the failure is a hard SIGSEGV with no
diagnostic, and it is a *silent ABI mismatch* -- the class that hides behind a
function-pointer cast the C compiler cannot see through. Any `^fat` sink over a
function returning a 9..16 byte aggregate is affected on Windows.

**Platform:** Windows x64 only. SysV (Linux/macOS x86-64) is correct as written.

## Repro

`tests/fixtures/fat-dispatch-parametric-monomorph-return` (now carrying
`requires.win64-aggregate-abi`, so it PASS-skips on Windows):

```sh
tur build tests/fixtures/fat-dispatch-parametric-monomorph-return/input.tur -o fd.exe
./fd.exe        # no output at all, SIGSEGV
```

```
Thread 1 received signal SIGSEGV
#0  0x00000000005ffec0 in ?? ()
#1  0x00007ff6ed23355e in main ()
```

It dies on the **first** of the three calls -- `(apply2 mk 7)`, whose
`(Box2 int)` is 16 bytes -- before printing anything. Expected `77 / 78.1 / 30`.

## Root cause

[src/compiler/emit_expr.c](../../src/compiler/emit_expr.c):

```c
bool adt_app_byval_pass_by_ptr(Type t) {
    return adt_app_byval_value_size_bytes(t) > 16;
}
```

Consulted by `thunk_type_has_concrete_c_abi`
([emit_module.c:330](../../src/compiler/emit_module.c)) to choose, in result
position, between the typed fatshim and the generic
`int64_t (*)(void *, int64_t...)` forwarding shim.

The fixture's own header explains the mechanism exactly -- it just assumes the
SysV boundary:

> Past 16 bytes SysV switches the return to the hidden-pointer (sret)
> convention, which shifts every argument right by one: the shim reads the
> caller's sret destination as its env and the program jumps to 0.

On Win64 that boundary is 8, not 16, so the three widths reclassify:

| type | size | SysV | Win64 |
| --- | --- | --- | --- |
| `(Box2 int)` | 16 | RAX:RDX | **sret** |
| `(BoxF float)` | 16 | XMM0:XMM1 | **sret** |
| `(Big3 int)` | 24 | sret | sret |

Only the last one is classified correctly today, which is why the crash lands
on the first call rather than the third.

## Fix directions

The hard part is not the predicate, it is *where the decision may be made*.
WIN1's rule is that the emitted C stays portable -- the platform split lives in
the OUTPUT as `#ifdef`, not in whichever host ran `emit-c` -- so a snapshot
generated on Linux must still compile and run correctly on Windows. A plain
`#ifdef _WIN32` inside `adt_app_byval_pass_by_ptr` would bake the host's ABI
into the generated C and break that.

1. **Emit both and let the preprocessor choose.** Emit the typed shim and the
   generic shim, selecting the slot-0 initializer under `#ifdef _WIN32`. Keeps
   the C portable; costs one dead shim per site.
2. **Always emit the typed shim** for a concrete aggregate result. Simplest, but
   the comment at emit_module.c:388-412 documents why the generic shim is
   currently required at <= 16 bytes: a rank-2 erased consumer calls slot 0
   through the erased cast, and a typed aggregate-returning shim there is UB
   that c2mir turns into a real wrong-sret crash. Would need that hazard
   re-examined, not just overridden.
3. **Make the threshold a target property** threaded from a target descriptor
   rather than a constant, and emit the selection accordingly. The cleanest
   long-term shape, and the largest change -- there is no target-descriptor
   concept in the emitter today.

Whichever route, the fixture already spells all three widths on purpose, so
un-skipping it on Windows is the check.

## Related

- [windows-longjmp-remaining-fiber-sites.md](windows-longjmp-remaining-fiber-sites.md)
- [docs/upcoming/v1/windows-remaining-plan.md](../upcoming/v1/windows-remaining-plan.md)
