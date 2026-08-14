#!/usr/bin/env python3
"""
rewrite_fn_type_colons.py -- drop redundant leading colons inside `(fn ...)`
type expressions.

Inside a `(fn [P...] R)` *type* expression, position alone tells the
elaborator which inner forms are parameter types and which is the result
type, so the leading ':' on each inner type is redundant:

    (fn [:int] :int)              ->  (fn [int] int)
    (fn [:int :int] #{} :bool)    ->  (fn [int int] #{} bool)
    (fn [:int] (fn [:int] :int))  ->  (fn [int] (fn [int] int))

Only `(fn ...)` forms that appear in a *type position* are rewritten --
never a `(fn ...)` lambda *value*.  Type positions are located
structurally: defn/fn parameter-type and return-type slots, defstruct
field types, let/loop binding types, defalias bodies, definstance/
defclass/defprotocol method signatures, and `(:: expr TYPE)` /
`(the TYPE expr)` ascriptions -- plus, recursively, the slots of any
`(fn ...)` type reached from one of those.

This is the inner-type-expression analogue of `spaced-types-rewrite.py`
and shares its tokenizer/parser.  See
docs/archive/still-in-flight-plan.md ("Drop leading colons inside
`(fn ...)` types").

Usage:
  tools/rewrite_fn_type_colons.py [opts] PATH [PATH...]

Modes (mutually exclusive):
  --write     (default) Rewrite files in place.
  --dry-run   Print unified diff; do not modify.
  --check     Exit 1 if any file would be rewritten.

`.md` files are handled by rewriting only fenced ```turmeric / ```tur
(and ```sweet-exp) code blocks; surrounding prose is left untouched.

Sweet-exp coverage: a *sweet-exp* top-level form (`defn foo [...] ...`
with no enclosing parens) is recognised via an implicit-sequence walk,
the same shape `spaced-types-rewrite.py` carries.  It is enabled
automatically when a `#lang sweet-exp` directive is present, for every
`.tur.sweet` file, and for ```sweet-exp markdown fences.  Inner `(fn ...)`
type forms are still explicitly paren-delimited in sweet-exp, so only the
outer declaration head needs the flat-sequence treatment; type forms only
reachable by deeper indentation than the top-level scan visits are missed
(never corrupted) -- the legacy `:int` spelling still parses.
"""

from __future__ import annotations

import argparse
import difflib
import os
import re
import sys
from dataclasses import dataclass, field
from typing import Iterable, Optional


# -------------------- Tokenizer (shared shape with spaced-types-rewrite.py) --

TK_LPAREN     = "LPAREN"
TK_RPAREN     = "RPAREN"
TK_LBRACKET   = "LBRACKET"
TK_RBRACKET   = "RBRACKET"
TK_LBRACE     = "LBRACE"
TK_RBRACE     = "RBRACE"
TK_HASH_LBRACE = "HASH_LBRACE"
TK_DISPATCH_LBRACE = "DISPATCH_LBRACE"
TK_QUOTE      = "QUOTE"
TK_QUASIQUOTE = "QUASIQUOTE"
TK_UNQUOTE    = "UNQUOTE"
TK_UNQUOTE_SPLICE = "UNQUOTE_SPLICE"
TK_STRING     = "STRING"
TK_CHAR       = "CHAR"
TK_CODE_FENCE = "CODE_FENCE"
TK_COMMENT    = "COMMENT"
TK_NEWLINE    = "NEWLINE"
TK_WS         = "WS"
TK_KEYWORD    = "KEYWORD"
TK_SYMBOL     = "SYMBOL"
TK_NUMBER     = "NUMBER"
TK_HASH_ATOM  = "HASH_ATOM"
TK_LANG_LINE  = "LANG_LINE"
TK_SHEBANG    = "SHEBANG"

TRIVIA_KINDS = {TK_WS, TK_NEWLINE, TK_COMMENT}


@dataclass
class Tok:
    kind: str
    text: str
    value: Optional[str] = None
    rewritten: bool = False


def _is_atom_char(c: str) -> bool:
    if c in "()[]{}\"`'~,;\\":
        return False
    if c.isspace():
        return False
    return True


def tokenize(src: str) -> list[Tok]:
    toks: list[Tok] = []
    i = 0
    n = len(src)
    line_start = True

    while i < n:
        c = src[i]

        if i == 0 and src.startswith("#!"):
            end = src.find("\n", i)
            if end < 0:
                end = n
            toks.append(Tok(TK_SHEBANG, src[i:end]))
            i = end
            continue

        if line_start and src.startswith("#lang", i):
            end = src.find("\n", i)
            if end < 0:
                end = n
            toks.append(Tok(TK_LANG_LINE, src[i:end]))
            i = end
            continue

        if c == "\n":
            toks.append(Tok(TK_NEWLINE, "\n"))
            i += 1
            line_start = True
            continue

        if c.isspace():
            j = i
            while j < n and src[j] != "\n" and src[j].isspace():
                j += 1
            toks.append(Tok(TK_WS, src[i:j]))
            i = j
            continue

        if c == ";":
            j = i
            while j < n and src[j] != "\n":
                j += 1
            toks.append(Tok(TK_COMMENT, src[i:j]))
            i = j
            continue

        if c == '"':
            j = i + 1
            while j < n:
                if src[j] == "\\" and j + 1 < n:
                    j += 2
                    continue
                if src[j] == '"':
                    j += 1
                    break
                j += 1
            toks.append(Tok(TK_STRING, src[i:j]))
            i = j
            line_start = False
            continue

        if c == "\\":
            j = i + 1
            if j < n:
                j += 1
            while j < n and _is_atom_char(src[j]):
                j += 1
            toks.append(Tok(TK_CHAR, src[i:j]))
            i = j
            line_start = False
            continue

        if src.startswith("```", i):
            j = i + 3
            close = src.find("```", j)
            if close < 0:
                close = n
            else:
                close += 3
            toks.append(Tok(TK_CODE_FENCE, src[i:close]))
            i = close
            line_start = False
            continue

        if c == "(":
            toks.append(Tok(TK_LPAREN, "("));  i += 1; line_start = False; continue
        if c == ")":
            toks.append(Tok(TK_RPAREN, ")"));  i += 1; line_start = False; continue
        if c == "[":
            toks.append(Tok(TK_LBRACKET, "[")); i += 1; line_start = False; continue
        if c == "]":
            toks.append(Tok(TK_RBRACKET, "]")); i += 1; line_start = False; continue
        if c == "{":
            toks.append(Tok(TK_LBRACE, "{"));  i += 1; line_start = False; continue
        if c == "}":
            toks.append(Tok(TK_RBRACE, "}"));  i += 1; line_start = False; continue

        if c == "#":
            if i + 1 < n and src[i + 1] == "{":
                toks.append(Tok(TK_HASH_LBRACE, "#{"))
                i += 2
                line_start = False
                continue
            j = i + 1
            while j < n and _is_atom_char(src[j]) and src[j] != "{":
                j += 1
            if j < n and src[j] == "{":
                toks.append(Tok(TK_DISPATCH_LBRACE, src[i:j + 1]))
                i = j + 1
                line_start = False
                continue
            j = i + 1
            while j < n and _is_atom_char(src[j]):
                j += 1
            toks.append(Tok(TK_HASH_ATOM, src[i:j]))
            i = j
            line_start = False
            continue

        if c == "'":
            toks.append(Tok(TK_QUOTE, "'"));  i += 1; line_start = False; continue
        if c == "`":
            toks.append(Tok(TK_QUASIQUOTE, "`")); i += 1; line_start = False; continue
        if c == "~" or c == ",":
            if i + 1 < n and src[i + 1] == "@":
                toks.append(Tok(TK_UNQUOTE_SPLICE, src[i:i + 2]))
                i += 2
            else:
                toks.append(Tok(TK_UNQUOTE, c))
                i += 1
            line_start = False
            continue

        if c == ":":
            j = i + 1
            while j < n and _is_atom_char(src[j]):
                j += 1
            if j == i + 1:
                toks.append(Tok(TK_SYMBOL, ":", value=":"))
                i = j
                line_start = False
                continue
            text = src[i:j]
            toks.append(Tok(TK_KEYWORD, text, value=text))
            i = j
            line_start = False
            continue

        if c.isdigit() or (c in "+-." and i + 1 < n and src[i + 1].isdigit()):
            j = i
            while j < n and _is_atom_char(src[j]):
                j += 1
            text = src[i:j]
            if any(ch.isdigit() for ch in text) and all(
                ch.isdigit() or ch in "+-._eExX0123456789abcdefABCDEF" for ch in text
            ):
                toks.append(Tok(TK_NUMBER, text, value=text))
            else:
                toks.append(Tok(TK_SYMBOL, text, value=text))
            i = j
            line_start = False
            continue

        j = i
        while j < n and _is_atom_char(src[j]):
            j += 1
        if j == i:
            toks.append(Tok(TK_SYMBOL, src[i:i + 1], value=src[i:i + 1]))
            i += 1
        else:
            text = src[i:j]
            toks.append(Tok(TK_SYMBOL, text, value=text))
            i = j
        line_start = False

    return toks


# -------------------- Form tree --------------------

@dataclass
class Form:
    open: Tok
    close: Optional[Tok]
    children: list = field(default_factory=list)

    @property
    def open_kind(self) -> str:
        return self.open.kind

    def is_paren(self) -> bool:    return self.open_kind == TK_LPAREN
    def is_bracket(self) -> bool:  return self.open_kind == TK_LBRACKET
    def is_brace(self) -> bool:    return self.open_kind == TK_LBRACE
    def is_hash_brace(self) -> bool: return self.open_kind == TK_HASH_LBRACE
    def is_dispatch_brace(self) -> bool: return self.open_kind == TK_DISPATCH_LBRACE


OPEN_TO_CLOSE = {
    TK_LPAREN: TK_RPAREN,
    TK_LBRACKET: TK_RBRACKET,
    TK_LBRACE: TK_RBRACE,
    TK_HASH_LBRACE: TK_RBRACE,
    TK_DISPATCH_LBRACE: TK_RBRACE,
}


def parse(tokens: list[Tok]) -> list:
    pos = [0]

    def parse_seq(stop_kinds: set[str]) -> list:
        out: list = []
        while pos[0] < len(tokens):
            t = tokens[pos[0]]
            if t.kind in stop_kinds:
                return out
            if t.kind in OPEN_TO_CLOSE:
                pos[0] += 1
                close_kind = OPEN_TO_CLOSE[t.kind]
                children = parse_seq({close_kind})
                close = None
                if pos[0] < len(tokens) and tokens[pos[0]].kind == close_kind:
                    close = tokens[pos[0]]
                    pos[0] += 1
                out.append(Form(open=t, close=close, children=children))
            else:
                out.append(t)
                pos[0] += 1
        return out

    return parse_seq(set())


# -------------------- Tree helpers --------------------

def code_children(form_or_list) -> list:
    """Children with trivia + reader-macro prefixes filtered, paired with index."""
    src = form_or_list.children if isinstance(form_or_list, Form) else form_or_list
    out = []
    for i, node in enumerate(src):
        if isinstance(node, Tok):
            if node.kind in TRIVIA_KINDS:
                continue
            if node.kind in (TK_QUOTE, TK_QUASIQUOTE, TK_UNQUOTE, TK_UNQUOTE_SPLICE):
                continue
        out.append((i, node))
    return out


def head_symbol(form: Form) -> Optional[str]:
    if not form.is_paren():
        return None
    for _, node in code_children(form):
        if isinstance(node, Tok) and node.kind == TK_SYMBOL:
            return node.value
        return None
    return None


# -------------------- Rewriter --------------------

DEFN_HEADS    = {"defn", "defmacro", "fn"}
STRUCT_HEADS  = {"defstruct"}
LET_HEADS     = {"let", "let*", "loop"}
INSTANCE_HEADS = {"definstance", "defclass", "defprotocol"}
ALIAS_HEADS   = {"defalias", "deftype"}
ASCRIBE_LAST  = {"::"}      # (:: expr TYPE)  -- type is the LAST argument
ASCRIBE_FIRST = {"the"}     # (the TYPE expr) -- type is the FIRST argument


@dataclass
class Stats:
    rewrites: int = 0


def strip_leading_colon(t: Tok, stats: Stats) -> None:
    """Convert a `:foo` KEYWORD token in fn-type slot into the bare `foo`."""
    if t.kind != TK_KEYWORD or t.rewritten or len(t.text) < 2 or not t.text.startswith(":"):
        return
    t.kind = TK_SYMBOL
    t.text = t.text[1:]
    t.value = t.text
    t.rewritten = True
    stats.rewrites += 1


def rewrite_nested_fns(node, stats: Stats) -> None:
    """Within a type context, descend a non-fn type form to rewrite any
    nested `(fn ...)` type, without stripping the enclosing form's own
    direct colons (out of scope for this codemod)."""
    if not isinstance(node, Form):
        return
    if node.is_paren() and head_symbol(node) == "fn":
        rewrite_fn_type(node, stats)
        return
    for child in node.children:
        if isinstance(child, Form):
            rewrite_nested_fns(child, stats)


def process_type_slot(node, stats: Stats) -> None:
    """A node sitting directly in a known type position."""
    if isinstance(node, Tok):
        return  # a bare/keyword simple type -- colon spacing is not our concern
    if not isinstance(node, Form):
        return
    if node.is_paren() and head_symbol(node) == "fn":
        rewrite_fn_type(node, stats)
    else:
        rewrite_nested_fns(node, stats)


def _strip_slot_seq(items: list, start: int, stats: Stats) -> int:
    """Strip the redundant leading colon on one inner-type form starting at
    `items[start]` (a list of (idx, node) code items inside an fn type), and
    recurse into it.  Returns the index just past the consumed type form.

    Three spellings are handled:
      * `:int`          -- a fused KEYWORD; drop its leading colon.
      * `: (Foo bar)`   -- a bare `:` SYMBOL then a form; blank the `:`.
      * `(Foo bar)`/Foo -- already bare; just recurse.
    """
    _, node = items[start]
    if isinstance(node, Tok) and node.kind == TK_KEYWORD:
        strip_leading_colon(node, stats)
        return start + 1
    if isinstance(node, Tok) and node.kind == TK_SYMBOL and node.value == ":":
        # bare `:` introducing the following type form -- delete the colon.
        if start + 1 < len(items):
            node.text = ""
            node.value = ""
            node.rewritten = True
            stats.rewrites += 1
            process_type_slot(items[start + 1][1], stats)
            return start + 2
        return start + 1
    process_type_slot(node, stats)
    return start + 1


def rewrite_fn_type(form: Form, stats: Stats) -> None:
    """`form` is a `(fn [P...] #{eff}? R)` *type*.  Strip leading colons on
    each parameter type and on the result type, recursing into nested
    `(fn ...)` types."""
    code = code_children(form)
    idx = 1  # skip the `fn` head
    # Parameter-type vector.
    if idx < len(code):
        _, node = code[idx]
        if isinstance(node, Form) and node.is_bracket():
            pitems = code_children(node)
            pi = 0
            while pi < len(pitems):
                pi = _strip_slot_seq(pitems, pi, stats)
            idx += 1
    # Optional effect/contract sets.
    while idx < len(code):
        _, node = code[idx]
        if isinstance(node, Form) and node.is_hash_brace():
            idx += 1
            continue
        break
    # Result type (single form, possibly introduced by a bare `:`).
    if idx < len(code):
        _strip_slot_seq(code, idx, stats)


def _process_signature_items(code: list, start: int, stats: Stats):
    """Slot-finding core of a defn/fn/method signature, decoupled from
    `Form` so both the paren walk and the implicit-sequence (sweet-exp)
    walk can share it.  `code` is the list of `(orig_index, node)` code
    items; `start` is the index of the first leading bracket (i.e. past the
    head symbol and any name).  Rewrites the param-vec and return-type
    slots; returns `(leading_brackets, ret_form)` so a Form-based caller can
    skip them when walking the body."""
    idx = start
    # Collect leading bracket vectors (type-params then params).  Any
    # `(fn ...)` directly inside a parameter bracket is a type annotation.
    leading_brackets = []
    while idx < len(code):
        _, node = code[idx]
        if isinstance(node, Form) and node.is_bracket():
            leading_brackets.append(node)
            idx += 1
        else:
            break
    for vec in leading_brackets:
        for _, child in code_children(vec):
            if isinstance(child, Form):
                process_type_slot(child, stats)
    # Return-type slot: skip effect sets, then a bare `:` introduces the
    # return type as the following form.  A fused `:kw` return type can
    # never be an fn type.
    ret_form = None
    while idx < len(code):
        _, node = code[idx]
        if isinstance(node, Form) and node.is_hash_brace():
            idx += 1
            continue
        if isinstance(node, Tok) and node.kind == TK_SYMBOL and node.value == ":":
            if idx + 1 < len(code):
                _, rt = code[idx + 1]
                process_type_slot(rt, stats)
                if isinstance(rt, Form):
                    ret_form = rt
            break
        break
    return leading_brackets, ret_form


def _process_signature(form: Form, stats: Stats, name_slots: int) -> None:
    """defn/fn/method signature shape:
        (HEAD NAME? [TYPEPARAMS]? [PARAMS] #{eff}? :RET? body...)
    `name_slots` is how many leading code items to skip before the param
    bracket(s): 1 for the head symbol, +1 if a name follows it."""
    code = code_children(form)
    leading_brackets, ret_form = _process_signature_items(code, name_slots, stats)
    # Body: walk every remaining child form except the param brackets and
    # the return-type form.
    for child in form.children:
        if isinstance(child, Form) and child not in leading_brackets and child is not ret_form:
            walk(child, stats)


def _process_struct_field_vec(vec: Form, stats: Stats) -> None:
    """Rewrite fn types in a defstruct field bracket: every Form child sits
    in a field-type slot."""
    for _, child in code_children(vec):
        if isinstance(child, Form):
            process_type_slot(child, stats)


def _process_let_binding_vec(vec: Form, stats: Stats) -> None:
    """Rewrite fn types in a let/loop binding bracket: a Form preceded by a
    bare `:` is a binding type annotation; otherwise it is a value form to
    walk normally."""
    items = code_children(vec)
    for k, (_, node) in enumerate(items):
        if not isinstance(node, Form):
            continue
        prev = items[k - 1][1] if k > 0 else None
        is_type = (
            isinstance(prev, Tok)
            and prev.kind == TK_SYMBOL
            and prev.value == ":"
        )
        if is_type:
            process_type_slot(node, stats)
        else:
            walk(node, stats)


def _process_struct(form: Form, stats: Stats) -> None:
    code = code_children(form)
    idx = 2  # skip head + name
    while idx < len(code):
        _, node = code[idx]
        if isinstance(node, Form) and node.is_bracket():
            _process_struct_field_vec(node, stats)
            idx += 1
            break
        idx += 1
    for child in form.children:
        if isinstance(child, Form) and not child.is_bracket():
            walk(child, stats)


def _process_let(form: Form, stats: Stats) -> None:
    code = code_children(form)
    idx = 1
    # named-let: optional symbol before the binding vector.
    if idx < len(code):
        _, node = code[idx]
        if isinstance(node, Tok) and node.kind == TK_SYMBOL:
            idx += 1
    bind_vec = None
    if idx < len(code):
        _, node = code[idx]
        if isinstance(node, Form) and node.is_bracket():
            bind_vec = node
            idx += 1
    if bind_vec is not None:
        _process_let_binding_vec(bind_vec, stats)
    # Body forms.
    for child in form.children:
        if isinstance(child, Form) and child is not bind_vec:
            walk(child, stats)


def _process_instance(form: Form, stats: Stats) -> None:
    for child in form.children:
        if isinstance(child, Form) and child.is_paren():
            # Each inner paren is a method signature: (name [params] :ret body?).
            _process_signature(child, stats, name_slots=1)
        elif isinstance(child, Form):
            walk(child, stats)


def _process_alias(form: Form, stats: Stats) -> None:
    code = code_children(form)
    if len(code) >= 3:
        process_type_slot(code[2][1], stats)


def _process_ascribe(form: Form, stats: Stats, where: str) -> None:
    code = code_children(form)
    body = [n for _, n in code[1:]]
    if not body:
        return
    if where == "last":
        type_node = body[-1]
        value_nodes = body[:-1]
    else:  # first
        type_node = body[0]
        value_nodes = body[1:]
    process_type_slot(type_node, stats)
    for v in value_nodes:
        if isinstance(v, Form):
            walk(v, stats)


def walk(form, stats: Stats) -> None:
    if not isinstance(form, Form):
        return
    if form.is_paren():
        head = head_symbol(form)
        if head in DEFN_HEADS:
            _process_signature(form, stats, name_slots=2 if head != "fn" else 1)
            return
        if head in STRUCT_HEADS:
            _process_struct(form, stats)
            return
        if head in LET_HEADS:
            _process_let(form, stats)
            return
        if head in INSTANCE_HEADS:
            _process_instance(form, stats)
            return
        if head in ALIAS_HEADS:
            _process_alias(form, stats)
            return
        if head in ASCRIBE_LAST:
            _process_ascribe(form, stats, "last")
            return
        if head in ASCRIBE_FIRST:
            _process_ascribe(form, stats, "first")
            return
    for child in form.children:
        if isinstance(child, Form):
            walk(child, stats)


# -------------------- Serialization --------------------

def serialize(nodes) -> str:
    out: list[str] = []

    def visit(node):
        if isinstance(node, Tok):
            out.append(node.text)
        else:
            out.append(node.open.text)
            for c in node.children:
                visit(c)
            if node.close is not None:
                out.append(node.close.text)

    for n in nodes:
        visit(n)
    return "".join(out)


def _walk_implicit_seq(nodes: list, stats: Stats) -> None:
    """Walk a flat sequence of nodes looking for sweet-exp top-level forms
    that have no surrounding paren -- e.g. `defn NAME [VEC] :RET` directly at
    file scope.  Matches the same head symbols as `walk` and applies the
    fn-type slot processors to the immediately-following items.

    Inner `(fn ...)` type forms are still explicitly paren-delimited, so the
    flat scan only has to locate the outer declaration head; the per-slot
    rewriter is shared verbatim with the paren walk.  Body expressions are
    paren/bracket-delimited siblings, so they are reached by the normal
    paren walk -- this pass does not descend into them."""
    code = []
    for i, node in enumerate(nodes):
        if isinstance(node, Tok) and node.kind in TRIVIA_KINDS:
            continue
        code.append((i, node))
    n = len(code)
    ci = 0
    while ci < n:
        _, node = code[ci]
        if not (isinstance(node, Tok) and node.kind == TK_SYMBOL):
            ci += 1
            continue
        head = node.value
        if head in DEFN_HEADS:
            start = ci + 1
            # defn/defmacro carry a name before the param vec; fn does not.
            if head != "fn" and start < n:
                _, name_node = code[start]
                if isinstance(name_node, Tok) and name_node.kind == TK_SYMBOL:
                    start += 1
            _process_signature_items(code, start, stats)
            ci = start
            continue
        if head in STRUCT_HEADS:
            start = ci + 1
            if start < n:  # skip name
                start += 1
            while start < n:
                _, nn = code[start]
                if isinstance(nn, Form) and nn.is_bracket():
                    _process_struct_field_vec(nn, stats)
                    start += 1
                    break
                start += 1
            ci = start
            continue
        if head in LET_HEADS:
            start = ci + 1
            if start < n:  # optional named-let name
                _, nn = code[start]
                if isinstance(nn, Tok) and nn.kind == TK_SYMBOL:
                    start += 1
            if start < n:
                _, nn = code[start]
                if isinstance(nn, Form) and nn.is_bracket():
                    _process_let_binding_vec(nn, stats)
                    start += 1
            ci = start
            continue
        if head in ALIAS_HEADS:
            # defalias/deftype NAME TYPE
            start = ci + 1
            if start < n:  # skip name
                start += 1
            if start < n:
                process_type_slot(code[start][1], stats)
                start += 1
            ci = start
            continue
        ci += 1


def rewrite_source(src: str, force_sweet: bool = False) -> tuple[str, int]:
    toks = tokenize(src)
    tree = parse(toks)
    stats = Stats()
    is_sweet = force_sweet or any(
        isinstance(t, Tok) and t.kind == TK_LANG_LINE and "sweet-exp" in t.text
        for t in tree
        if isinstance(t, Tok)
    )
    for node in tree:
        if isinstance(node, Form):
            walk(node, stats)
    if is_sweet:
        _walk_implicit_seq(tree, stats)
    return serialize(tree), stats.rewrites


# -------------------- Markdown support --------------------

_FENCE_RE = re.compile(
    r"(?P<fence>^[ \t]*```)(?P<info>[^\n]*)\n(?P<body>.*?)(?P<close>^[ \t]*```)",
    re.DOTALL | re.MULTILINE,
)

_TUR_INFO = {"turmeric", "tur", "scheme", "lisp", "clojure"}
_SWEET_INFO = {"sweet-exp", "sweet"}


def rewrite_markdown(src: str) -> tuple[str, int]:
    total = 0

    def repl(m: re.Match) -> str:
        nonlocal total
        info = m.group("info").strip().lower()
        if info in _TUR_INFO:
            new_body, n = rewrite_source(m.group("body"))
        elif info in _SWEET_INFO:
            new_body, n = rewrite_source(m.group("body"), force_sweet=True)
        else:
            return m.group(0)
        total += n
        return m.group("fence") + m.group("info") + "\n" + new_body + m.group("close")

    new_src = _FENCE_RE.sub(repl, src)
    return new_src, total


# -------------------- Driver --------------------

def iter_files(paths: Iterable[str]) -> Iterable[str]:
    for p in paths:
        if os.path.isdir(p):
            for root, _, files in os.walk(p):
                for f in files:
                    if f.endswith((".tur", ".tur.sweet", ".md")):
                        yield os.path.join(root, f)
        else:
            yield p


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="+", help="files or directories to process")
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--write", action="store_true", help="rewrite files in place (default)")
    mode.add_argument("--dry-run", action="store_true", help="print unified diff; do not write")
    mode.add_argument("--check", action="store_true", help="exit 1 if any file would be rewritten")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    if not (args.dry_run or args.check):
        args.write = True

    any_changed = False
    total_rewrites = 0
    n_files_changed = 0

    for path in iter_files(args.paths):
        try:
            with open(path, "r", encoding="utf-8") as fh:
                src = fh.read()
        except (OSError, UnicodeDecodeError) as e:
            print(f"{path}: read error: {e}", file=sys.stderr)
            return 2
        try:
            if path.endswith(".md"):
                new_src, n = rewrite_markdown(src)
            elif path.endswith(".tur.sweet"):
                new_src, n = rewrite_source(src, force_sweet=True)
            else:
                new_src, n = rewrite_source(src)
        except Exception as e:
            print(f"{path}: rewrite error: {e}", file=sys.stderr)
            return 2
        if new_src != src:
            any_changed = True
            n_files_changed += 1
            total_rewrites += n
            if args.check:
                print(f"{path}: {n} fn-type colon(s) need rewrite", file=sys.stderr)
            elif args.dry_run:
                diff = difflib.unified_diff(
                    src.splitlines(keepends=True),
                    new_src.splitlines(keepends=True),
                    fromfile=f"a/{path}", tofile=f"b/{path}",
                )
                sys.stdout.writelines(diff)
            else:
                with open(path, "w", encoding="utf-8") as fh:
                    fh.write(new_src)
                if args.verbose:
                    print(f"rewrote {path} ({n} rewrites)", file=sys.stderr)
        elif args.verbose:
            print(f"clean {path}", file=sys.stderr)

    if args.check:
        if any_changed:
            print(f"{n_files_changed} file(s) need rewrite, {total_rewrites} colon(s) total",
                  file=sys.stderr)
            return 1
        return 0
    if any_changed:
        verb = "would change" if args.dry_run else "rewritten"
        print(f"{n_files_changed} file(s) {verb}, {total_rewrites} rewrite(s) total",
              file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
