# turmeric_lldb.py -- lldb pretty-printers that render Turmeric values as
# Turmeric, not as the generated C carrier.
#
# Part of the debugger Phase 5 (rich type display in native debuggers) track.
# Layered on the ordinary C-DWARF the C compiler already emits from
# `tur --debug build` output.
#
# Load it inside lldb:
#
#   command script import tools/debug/turmeric_lldb.py
#
# or add that command to your ~/.lldbinit.

import lldb


def _scalar(val):
    """Render a carrier field, falling back through summaries and raw values."""
    if not val.IsValid():
        return "?"
    summary = val.GetSummary()
    if summary:
        return summary
    value = val.GetValue()
    if value is not None:
        return value
    desc = val.GetObjectDescription()
    if desc:
        return desc
    return str(val)


def Option_SummaryProvider(valobj, internal_dict):
    """Option[A] summary provider."""
    # Resolve any pointers/references to the underlying value
    if valobj.GetType().IsPointerType() or valobj.GetType().IsReferenceType():
        valobj = valobj.Dereference()
    
    is_some_child = valobj.GetChildMemberWithName('is_some')
    if not is_some_child.IsValid():
        return "(option ?)"
    is_some = bool(is_some_child.GetValueAsUnsigned())
    if is_some:
        val_child = valobj.GetChildMemberWithName('value')
        return "(some %s)" % _scalar(val_child)
    return "(none)"


def Result_SummaryProvider(valobj, internal_dict):
    """Result[A B] summary provider."""
    if valobj.GetType().IsPointerType() or valobj.GetType().IsReferenceType():
        valobj = valobj.Dereference()
        
    is_ok_child = valobj.GetChildMemberWithName('is_ok')
    if not is_ok_child.IsValid():
        return "(result ?)"
    is_ok = bool(is_ok_child.GetValueAsUnsigned())
    if is_ok:
        ok_child = valobj.GetChildMemberWithName('ok_val')
        return "(ok %s)" % _scalar(ok_child)
    else:
        err_child = valobj.GetChildMemberWithName('err_val')
        return "(err %s)" % _scalar(err_child)


def Cons_SummaryProvider(valobj, internal_dict):
    """Cons[A] cell summary provider."""
    if valobj.GetType().IsPointerType() or valobj.GetType().IsReferenceType():
        valobj = valobj.Dereference()
        
    head_child = valobj.GetChildMemberWithName('head')
    if not head_child.IsValid():
        return "(cons ?)"
    head = _scalar(head_child)
    tail_child = valobj.GetChildMemberWithName('tail')
    if tail_child.IsValid():
        tail_val = tail_child.GetValueAsUnsigned()
        more = " ..." if tail_val != 0 else ""
    else:
        more = ""
    return "(%s%s)" % (head, more)


def __lldb_init_module(debugger, internal_dict):
    """Register the pretty printers with LLDB."""
    # Register printers for Option, Result, and Cons types using regexes
    # that match the base names and monomorphized 'tur_adt_' prefixed variants.
    debugger.HandleCommand('type summary add -F turmeric_lldb.Option_SummaryProvider -x "^(tur_adt_)?Option(__.*)?$"')
    debugger.HandleCommand('type summary add -F turmeric_lldb.Result_SummaryProvider -x "^(tur_adt_)?Result(__.*)?$"')
    debugger.HandleCommand('type summary add -F turmeric_lldb.Cons_SummaryProvider -x "^(tur_adt_)?Cons(__.*)?$"')
