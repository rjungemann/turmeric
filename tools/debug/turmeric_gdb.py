# turmeric_gdb.py -- gdb pretty-printers that render Turmeric values as
# Turmeric, not as the generated C carrier.
#
# Part of the debugger Phase 5 (rich type display in native debuggers) track.
# Layered on the ordinary C-DWARF the C compiler already emits from
# `tur --debug build` output -- we do NOT emit DWARF directly from `tur`.
# Dispatch is by the deterministic C type names emit-c produces for the
# monomorphized by-value carriers:
#
#   Option / Option__<T>   -> (some <v>) | (none)
#   Result / Result__<T..> -> (ok <v>)   | (err <e>)
#   Cons   / Cons__<T>      -> (<h> ...)  cons-cell head (best-effort)
#
# Structs (Point{x,y}, ...) already read well as gdb's default aggregate
# display, so we deliberately do not shadow them.
#
# Load it explicitly:
#
#   gdb -ex "source tools/debug/turmeric_gdb.py" ./build/bin/foo
#
# or from your ~/.gdbinit / a project .gdbinit.  See
# docs/archive/history/debugger-native-types-plan.md (N2) for the auto-load story.

import gdb
import gdb.printing


def _scalar(val):
    """Render a carrier field, recursing through registered printers."""
    try:
        return val.format_string()
    except Exception:
        return str(val)


class OptionPrinter:
    """Option[A]  ->  (some <value>) | (none).

    SR2b: Option is a real sum -- the monomorph is the tagged union
    `{ int tag; union { struct {} None; struct { A _0; } Some; } as; }`
    with tag 0 = None, 1 = Some.  The retired record shape
    ({ bool is_some; A value; }) is still read as a fallback so the
    printer keeps working against an older binary."""

    def __init__(self, val):
        self.val = val

    def to_string(self):
        try:
            tag = int(self.val['tag'])
            if tag == 1:
                return "(some %s)" % _scalar(self.val['as']['Some']['_0'])
            return "(none)"
        except gdb.error:
            pass
        try:
            is_some = bool(self.val['is_some'])
        except gdb.error:
            return "(option ?)"
        if is_some:
            return "(some %s)" % _scalar(self.val['value'])
        return "(none)"


class ResultPrinter:
    """Result[A B]  ->  (ok <ok_val>) | (err <err_val>).

    SR2b tagged layout: tag 0 = Ok, 1 = Err, payload in as.Ok._0 /
    as.Err._0.  Retired record shape kept as a fallback (see
    OptionPrinter)."""

    def __init__(self, val):
        self.val = val

    def to_string(self):
        try:
            tag = int(self.val['tag'])
            if tag == 0:
                return "(ok %s)" % _scalar(self.val['as']['Ok']['_0'])
            return "(err %s)" % _scalar(self.val['as']['Err']['_0'])
        except gdb.error:
            pass
        try:
            is_ok = bool(self.val['is_ok'])
        except gdb.error:
            return "(result ?)"
        if is_ok:
            return "(ok %s)" % _scalar(self.val['ok_val'])
        return "(err %s)" % _scalar(self.val['err_val'])


class ConsPrinter:
    """Cons[A] cell  ->  (<head> ...) -- one cell, best-effort.

    A full list walk would follow `tail`, but by-value `Cons__<T>` cells are
    chained through an int64_t carrier tail, so the chain is not type-visible
    from a single cell.  We surface the head and signal the continuation.
    """

    def __init__(self, val):
        self.val = val

    def to_string(self):
        try:
            head = _scalar(self.val['head'])
        except gdb.error:
            return "(cons ?)"
        tail = self.val['tail']
        more = " ..." if int(tail) != 0 else ""
        return "(%s%s)" % (head, more)


def build_pretty_printer():
    pp = gdb.printing.RegexpCollectionPrettyPrinter("turmeric")
    # `^(tur_adt_)?Name(__...)?$` matches both the generic preamble typedef (`Option`)
    # and every monomorphized spec (including `tur_adt_Option__int`, `tur_adt_Result__int_cstr`, ...).
    pp.add_printer('Option', r'^(tur_adt_)?Option(__.*)?$', OptionPrinter)
    pp.add_printer('Result', r'^(tur_adt_)?Result(__.*)?$', ResultPrinter)
    pp.add_printer('Cons',   r'^(tur_adt_)?Cons(__.*)?$',   ConsPrinter)
    return pp


def register(objfile=None):
    gdb.printing.register_pretty_printer(
        objfile, build_pretty_printer(), replace=True)


# Registering against `None` installs globally (every objfile); that is what
# `source turmeric_gdb.py` and the `<binary>-gdb.py` auto-load sidecar both want.
register(None)
