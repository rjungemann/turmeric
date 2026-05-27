#!/usr/bin/env python3
"""
tools/gen_ffi_dispatch.py -- generate src/runtime/ffi_dispatch.{h,c}.

The dispatchers are tiny C trampolines that cast a `void *` function
pointer to the right C signature and call it with the supplied args.
They are what the REPL's FFI binding layer (RP4) will invoke once it has
dlsym'd an exported defn from a spice's shared library.

Each dispatcher covers one (return-class, arg-classes) shape:

    I = int64_t (covers Turmeric :int, :cstr, :bool, :ptr, sized ints,
                 and anything else that fits in an integer register --
                 the caller is responsible for casting before the call)
    F = double  (covers :float / :float64; :float32 must be widened by
                 the caller before invoking an F-arg dispatcher)
    V = void return only (the value class V is never an arg class)

Naming: `tur_ffi_call_<ret>_<args>` with lowercase class chars.
Examples:

    tur_ffi_call_v_           ; no-arg, no-return  -- ((void(*)(void))fn)()
    tur_ffi_call_i_           ; no-arg, int return
    tur_ffi_call_i_i          ; int -> int
    tur_ffi_call_i_iif        ; int int float -> int
    tur_ffi_call_f_ff         ; float float -> float
    tur_ffi_call_v_iii        ; int int int -> void

The trailing underscore on the no-arg case is intentional: it keeps the
parse rule "everything after the last underscore is the arg shape"
uniform.

Usage:

    # Regenerate with default coverage (arity 0..6 = 381 dispatchers).
    python3 tools/gen_ffi_dispatch.py

    # Explicit output paths (default: src/runtime/ffi_dispatch.{h,c}).
    python3 tools/gen_ffi_dispatch.py --out-c src/runtime/ffi_dispatch.c \
                                      --out-h src/runtime/ffi_dispatch.h

    # Cover a larger arity (e.g. an 8-arg defn appeared in a spice).
    python3 tools/gen_ffi_dispatch.py --max-arity 8

    # Extend the default set with shapes mined from exports.manifest
    # files. (Each manifest line ends in `(:a :b ...) -> :ret`; the script
    # maps those tags to I/F classes and merges them into the shape set.)
    python3 tools/gen_ffi_dispatch.py \
        --from-manifest spice-a/exports.manifest \
        --from-manifest spice-b/exports.manifest

The script is deterministic: shapes are sorted (by arity, then
lexicographic), and the same inputs always produce byte-identical
outputs, so the generated files are safe to commit.
"""

from __future__ import annotations

import argparse
import itertools
import re
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# Shape model
# ---------------------------------------------------------------------------

RET_CLASSES = ("v", "i", "f")
ARG_CLASSES = ("i", "f")

C_TYPE = {"v": "void", "i": "int64_t", "f": "double"}


def all_shapes(max_arity: int) -> list[tuple[str, str]]:
    """Enumerate every (ret_class, arg_classes) pair up to max_arity."""
    out: set[tuple[str, str]] = set()
    for arity in range(max_arity + 1):
        for args in itertools.product(ARG_CLASSES, repeat=arity):
            args_str = "".join(args)
            for ret in RET_CLASSES:
                out.add((ret, args_str))
    return sorted(out, key=lambda s: (len(s[1]), s[1], s[0]))


# ---------------------------------------------------------------------------
# Manifest parsing -- map :tag -> dispatcher class
# ---------------------------------------------------------------------------

# Tags that go in an integer register (or are returned in rax / x0).
INT_TAGS = {
    ":int", ":bool", ":cstr", ":ptr",
    ":int8", ":int16", ":int32", ":int64",
    ":uint8", ":uint16", ":uint32", ":uint64",
    # `:any` falls through as int64_t because the caller boxes/unboxes
    # values that don't fit the primitive set.
    ":any", ":never",
}
FLOAT_TAGS = {":float", ":float32", ":float64"}
VOID_TAGS = {":void"}

# Match `(:a :b ...) -> :ret` at the end of a manifest line.
SIGNATURE_RE = re.compile(r"::\s*\(([^)]*)\)\s*->\s*(:\S+)\s*$")


def tag_to_class(tag: str, *, is_return: bool) -> str | None:
    """Return 'i' / 'f' / 'v' (only valid for return) or None to skip."""
    if tag in INT_TAGS:
        return "i"
    if tag in FLOAT_TAGS:
        return "f"
    if is_return and tag in VOID_TAGS:
        return "v"
    return None


def shapes_from_manifest(path: Path) -> set[tuple[str, str]]:
    out: set[tuple[str, str]] = set()
    for raw in path.read_text().splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        # Strip the `& :rest-tag` from variadic signatures: the dispatcher
        # for the cons-list pointer arg is just one extra :int.
        # `[^)\s]+` (not `\S+`) so the substitution stops at the closing
        # paren -- otherwise `& :cstr)` would swallow the `)` and break
        # the signature regex below.
        line = re.sub(r"\s*&\s*:[^)\s]+", " :int", line)
        m = SIGNATURE_RE.search(line)
        if not m:
            print(f"warning: unparseable manifest line: {raw!r}",
                  file=sys.stderr)
            continue
        args_str = m.group(1).strip()
        ret_tag = m.group(2).strip()
        ret_class = tag_to_class(ret_tag, is_return=True)
        if ret_class is None:
            # Compound or struct return; skip until the FFI layer grows
            # support for it.
            continue
        arg_classes = []
        skip = False
        for tag in args_str.split():
            c = tag_to_class(tag, is_return=False)
            if c is None:
                skip = True
                break
            arg_classes.append(c)
        if skip:
            continue
        out.add((ret_class, "".join(arg_classes)))
    return out


# ---------------------------------------------------------------------------
# C emission
# ---------------------------------------------------------------------------

HEADER_PROLOGUE = """\
/* GENERATED by tools/gen_ffi_dispatch.py -- DO NOT EDIT BY HAND.
 *
 * Typed trampolines for calling foreign function pointers obtained via
 * dlsym. The Turmeric REPL (RP4) uses these to dispatch into compiled
 * spices: it picks the dispatcher whose name matches a defn's signature
 * shape (see tools/gen_ffi_dispatch.py for the encoding), then calls it
 * with the dlsym'd pointer plus the marshaled args.
 *
 * Class encoding (see the generator for the full rationale):
 *   v -- void return only
 *   i -- int64_t (covers :int, :cstr, :bool, :ptr, sized ints)
 *   f -- double  (covers :float / :float64)
 */
#ifndef TUR_RUNTIME_FFI_DISPATCH_H
#define TUR_RUNTIME_FFI_DISPATCH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

"""

HEADER_EPILOGUE = """\

#ifdef __cplusplus
}
#endif

#endif /* TUR_RUNTIME_FFI_DISPATCH_H */
"""

SOURCE_PROLOGUE = """\
/* GENERATED by tools/gen_ffi_dispatch.py -- DO NOT EDIT BY HAND. */
#include "ffi_dispatch.h"

"""

THUNK_PROLOGUE = """\
/* GENERATED by tools/gen_ffi_dispatch.py -- DO NOT EDIT BY HAND.
 *
 * tur_ffi_thunk_call -- shape-keyed bridge from the interpreter's
 * pre-marshaled scalar buffers (i_vals[] / f_vals[]) to the typed
 * trampolines in ffi_dispatch.h. Each (ret, args) combination is
 * lowered to a direct call to the corresponding tur_ffi_call_*
 * dispatcher.
 *
 * Pure-C (no TuriValue dependency) so the runtime stays decoupled
 * from the eval layer. Callers in src/turi/ffi_thunk.c marshal
 * TuriValue -> int64_t/double up front and wrap the result back
 * afterwards.
 */
#include "ffi_dispatch.h"
#include "ffi_dispatch_thunk.h"

#include <string.h>   /* memcmp -- used by the multi-arg shape branches */

int tur_ffi_thunk_call(char ret,
                       const char *args, uint8_t n_args,
                       void *fn,
                       const int64_t *i_vals,
                       const double  *f_vals,
                       int64_t *out_i,
                       double  *out_f)
{
"""

THUNK_EPILOGUE = """\
    /* No matching shape: dispatcher table didn't cover this combination.
     * In practice this is reachable only when n_args exceeds the
     * generator's --max-arity. Caller surfaces the user-facing error. */
    (void)fn; (void)i_vals; (void)f_vals; (void)out_i; (void)out_f;
    return -1;
}
"""

THUNK_HEADER = """\
/* GENERATED by tools/gen_ffi_dispatch.py -- DO NOT EDIT BY HAND.
 *
 * Shape-keyed bridge between the interpreter and ffi_dispatch.h.
 * See ffi_dispatch_thunk.c for the body and src/turi/ffi_thunk.c
 * for the marshaling layer that uses it.
 */
#ifndef TUR_RUNTIME_FFI_DISPATCH_THUNK_H
#define TUR_RUNTIME_FFI_DISPATCH_THUNK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Dispatch by shape. `ret` is 'i'/'f'/'v'; `args` is a string of
 * 'i'/'f' chars of length `n_args`. Pre-marshaled scalars live in
 * `i_vals` (only entries at indices where args[k]=='i' are valid)
 * and `f_vals` (likewise for 'f'). On success returns 0 and writes
 * the result to *out_i (ret='i') or *out_f (ret='f'), or leaves both
 * unchanged for ret='v'. Returns -1 if the combination isn't in the
 * generated table. */
int tur_ffi_thunk_call(char ret,
                       const char *args, uint8_t n_args,
                       void *fn,
                       const int64_t *i_vals,
                       const double  *f_vals,
                       int64_t *out_i,
                       double  *out_f);

#ifdef __cplusplus
}
#endif

#endif /* TUR_RUNTIME_FFI_DISPATCH_THUNK_H */
"""


def fn_name(ret: str, args: str) -> str:
    return f"tur_ffi_call_{ret}_{args}"


def c_signature(ret: str, args: str) -> str:
    """Return the dispatcher's prototype as it appears in code."""
    parts = ["void *fn"]
    for i, c in enumerate(args):
        parts.append(f"{C_TYPE[c]} a{i}")
    return f"{C_TYPE[ret]} {fn_name(ret, args)}({', '.join(parts)})"


def c_body(ret: str, args: str) -> str:
    """Emit the cast-and-call body."""
    fn_type_args = ", ".join(C_TYPE[c] for c in args) if args else "void"
    call_args = ", ".join(f"a{i}" for i in range(len(args)))
    cast = f"(({C_TYPE[ret]} (*)({fn_type_args}))fn)({call_args})"
    if ret == "v":
        return f"    {cast};\n"
    return f"    return {cast};\n"


def emit_header(shapes: list[tuple[str, str]]) -> str:
    buf = [HEADER_PROLOGUE]
    last_arity = -1
    for ret, args in shapes:
        if len(args) != last_arity:
            if last_arity != -1:
                buf.append("\n")
            buf.append(f"/* arity {len(args)} */\n")
            last_arity = len(args)
        buf.append(c_signature(ret, args) + ";\n")
    buf.append(HEADER_EPILOGUE)
    return "".join(buf)


def emit_source(shapes: list[tuple[str, str]]) -> str:
    buf = [SOURCE_PROLOGUE]
    for ret, args in shapes:
        buf.append(c_signature(ret, args) + " {\n")
        buf.append(c_body(ret, args))
        buf.append("}\n\n")
    return "".join(buf).rstrip() + "\n"


def emit_thunk(shapes: list[tuple[str, str]]) -> str:
    """Emit one big switch over (ret, args_str) -> typed dispatcher call.

    Shapes are grouped by `ret`, then by `n_args`, then by the exact
    `args_str`. The inner-most branch picks i_vals[k] vs f_vals[k]
    based on the shape's class chars, then calls the trampoline and
    stores the result in *out_i / *out_f (or nowhere for void).
    """
    buf = [THUNK_PROLOGUE]
    # Group by ret class.
    for ret in ("i", "f", "v"):
        ret_shapes = sorted(
            (s for s in shapes if s[0] == ret),
            key=lambda s: (len(s[1]), s[1]),
        )
        if not ret_shapes:
            continue
        buf.append(f"    if (ret == '{ret}') {{\n")
        # Group by arity.
        by_arity: dict[int, list[str]] = {}
        for _, args in ret_shapes:
            by_arity.setdefault(len(args), []).append(args)
        first_arity = True
        for arity in sorted(by_arity):
            buf.append(
                f"        {'if' if first_arity else 'else if'} (n_args == {arity}) {{\n"
            )
            first_arity = False
            arity_shapes = sorted(by_arity[arity])
            if arity == 0:
                # Only one shape: no arg-class branching.
                fn = fn_name(ret, "")
                call = f"{fn}(fn)"
                if ret == "v":
                    buf.append(f"            {call}; return 0;\n")
                elif ret == "i":
                    buf.append(f"            *out_i = {call}; return 0;\n")
                else:  # 'f'
                    buf.append(f"            *out_f = {call}; return 0;\n")
            else:
                # Switch on the packed arg-class string by chaining
                # `memcmp` checks; the generator is deterministic so
                # the chain order is stable.
                first_shape = True
                for args in arity_shapes:
                    fn = fn_name(ret, args)
                    call_args = []
                    for k, c in enumerate(args):
                        call_args.append(
                            f"i_vals[{k}]" if c == "i" else f"f_vals[{k}]"
                        )
                    call = f"{fn}(fn, {', '.join(call_args)})"
                    cond = (f"args[0] == '{args[0]}'"
                            if arity == 1
                            else "memcmp(args, \"" + args + "\", " + str(arity) + ") == 0")
                    keyword = "if" if first_shape else "else if"
                    first_shape = False
                    buf.append(f"            {keyword} ({cond}) {{\n")
                    if ret == "v":
                        buf.append(f"                {call}; return 0;\n")
                    elif ret == "i":
                        buf.append(f"                *out_i = {call}; return 0;\n")
                    else:
                        buf.append(f"                *out_f = {call}; return 0;\n")
                    buf.append("            }\n")
            buf.append("        }\n")
        buf.append("    }\n")
    buf.append(THUNK_EPILOGUE)
    return "".join(buf)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def parse_args(argv: list[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--max-arity", type=int, default=6,
                   help="enumerate every shape with arity 0..N (default: 6)")
    p.add_argument("--from-manifest", action="append", default=[],
                   metavar="PATH",
                   help="extend the shape set with shapes mined from an "
                        "exports.manifest (may be passed multiple times)")
    p.add_argument("--out-c", default="src/runtime/ffi_dispatch.c",
                   help="path for the generated .c (default: %(default)s)")
    p.add_argument("--out-h", default="src/runtime/ffi_dispatch.h",
                   help="path for the generated .h (default: %(default)s)")
    p.add_argument("--out-thunk-c",
                   default="src/runtime/ffi_dispatch_thunk.c",
                   help="path for the generated shape-switch thunk source")
    p.add_argument("--out-thunk-h",
                   default="src/runtime/ffi_dispatch_thunk.h",
                   help="path for the thunk header")
    return p.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    if args.max_arity < 0 or args.max_arity > 16:
        print("error: --max-arity must be between 0 and 16", file=sys.stderr)
        return 2

    shape_set = set(all_shapes(args.max_arity))
    for manifest_path in args.from_manifest:
        path = Path(manifest_path)
        if not path.exists():
            print(f"error: manifest not found: {manifest_path}",
                  file=sys.stderr)
            return 2
        shape_set |= shapes_from_manifest(path)

    shapes = sorted(shape_set, key=lambda s: (len(s[1]), s[1], s[0]))

    out_h = Path(args.out_h)
    out_c = Path(args.out_c)
    out_th = Path(args.out_thunk_h)
    out_tc = Path(args.out_thunk_c)
    for p in (out_h, out_c, out_th, out_tc):
        p.parent.mkdir(parents=True, exist_ok=True)
    out_h.write_text(emit_header(shapes))
    out_c.write_text(emit_source(shapes))
    out_th.write_text(THUNK_HEADER)
    out_tc.write_text(emit_thunk(shapes))

    print(f"wrote {out_h}, {out_c}, {out_th}, {out_tc} "
          f"({len(shapes)} dispatchers)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
