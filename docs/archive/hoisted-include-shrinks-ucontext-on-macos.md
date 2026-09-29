# macOS: a hoisted include gave FiberBlock the 56-byte ucontext_t

**RESOLVED 2026-09-29**, in the PR that added
`tests/fixtures/r7rs-threads-fiber-migration`.
`hoist_tur_include_directives` now includes `<ucontext.h>` under
`_XOPEN_SOURCE` ahead of the includes it hoists, on macOS. On macOS
`tur_fiber_block_new` also refuses to build a fiber from a `ucontext_t`
with no room for its machine context.

## Symptom

On macOS the fixture crashed with SIGSEGV on every run, with one worker
thread as with three, and with 256 KiB fiber stacks as with 4 MiB. It was
not the collector: runs that never collected crashed the same way. The crash
reports all had the same shape. A worker had jumped to pc 0 with
`x21 = 262144` (the fiber stack size) and `x22` holding a fresh fiber stack.
Those are the registers `getcontext` captures inside `tur_fiber_block_new`.
Meanwhile main was inside that function's `makecontext`, in `bzero`. The
worker had resumed a context that main was still writing.

## What was happening

On macOS the layout of `ucontext_t` is decided by the first header that
defines it (`<sys/_types/_ucontext.h>`, include-guarded). Under
`_XOPEN_SOURCE` it ends in `__mcontext_data`, the machine context, 880 bytes
in all on arm64. Defined without it, it is the 56-byte head alone. libc's
`getcontext` and `swapcontext` always write the machine context at the
`_XOPEN_SOURCE` offset, 816 bytes of it, whichever type the caller compiled
against.

The preamble knows this: it includes `<ucontext.h>` under `_XOPEN_SOURCE`
before `<setjmp.h>` and `<pthread.h>` (emit_module.c, "Phase T21"). But
`hoist_tur_include_directives` puts every `__tur_include__` directive above
the preamble. The r7rs prelude hoists `<setjmp.h>`, `<pthread.h>`,
`<stdint.h>` and `<stdlib.h>` for `call/cc`, and `<stdlib.h>` reaches
`<sys/signal.h>` through `<sys/wait.h>`. So every `#lang r7rs` program on
macOS had the 56-byte type. Each `FiberBlock` was about 1.6 KiB too small
for its two contexts. Every switch wrote over the fields after them and into
the next heap object, which was usually another `FiberBlock`.

Any program that hoists a system include and runs fibers was exposed. A
plain Turmeric program had the right layout, since nothing it included
reached `<sys/signal.h>` first. An earlier diagnostic that checked the
layout used the plain preamble's head and so found nothing wrong.

## Fix

- `hoist_tur_include_directives` (src/main.c) emits, right after its
  `_DEFAULT_SOURCE`, the same first steps as the preamble on `__APPLE__`:
  the BSD networking headers, while the full Darwin feature set is still the
  default, then `<ucontext.h>` under `_XOPEN_SOURCE`. The preamble's own
  copy is then a guarded no-op. Every hoist site goes through this function.
- `tur_fiber_block_new` aborts with a message on `__APPLE__` if
  `sizeof(ucontext_t)` has no room past the head for the machine context.
  That turns the next include-order mistake into a clear failure instead of
  heap corruption, and only in programs that make a fiber.

On Linux the type has one layout whatever the order, so nothing changes
there.
