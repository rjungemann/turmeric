#!/usr/bin/env bash
# tests/check-no-atomic-builtins.sh -- the emitted runtime never spells a GCC
# `__atomic_*` builtin directly; it goes through the preamble's TUR_ATOMIC_*
# layer (emit_module.c, "6(a) (jit-engine-plan section 4)").
#
# c2mir -- the JIT's C front end -- implements none of the `__atomic_*`
# family, so a literal one in the preamble makes EVERY `tur jit` program fail
# to link in the engine and fall back to cc (TUR-W0070).  That is not a crash
# anywhere a developer would look: `tur run` is unaffected, and most local
# builds carry no JIT.  It happened once (the threaded-async future's status
# store, 2026-09-26) and went unnoticed until a JIT build ran the corpus.
#
# Scope: string literals in the emitter source, which is where the preamble
# and runtime C come from.  The TUR_ATOMIC_* macro definitions are the one
# legitimate spelling -- their bodies call the builtin on the macro's own
# parameter `(p)`, which is how they are told apart.  Stdlib and user inline-C
# is out of scope: it is the program's own C, and a JIT fallback for it is a
# per-program capability miss the engine already reports.
set -euo pipefail
cd "$(dirname "$0")/.."

hits=$(git grep -nE '"[^"]*__atomic_[a-z_]+\(' -- 'src/compiler/*.c' \
        | grep -vE '__atomic_[a-z_]+\(\(p\)' || true)
if [ -n "$hits" ]; then
    echo "check-no-atomic-builtins: FAIL -- a literal __atomic_* builtin in emitted C:" >&2
    echo "$hits" | sed 's/^/    /' >&2
    echo "    use the TUR_ATOMIC_* macros (emit_module.c) so the JIT can compile it" >&2
    exit 1
fi
echo "check-no-atomic-builtins: OK -- the emitted runtime reaches atomics through TUR_ATOMIC_*"
