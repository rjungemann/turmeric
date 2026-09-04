#!/usr/bin/env python3
"""
spaced-types-rewrite.py -- migrate fused `:type` to spaced `: type`.

Rewrites Turmeric source files so type annotations in recognized
type-bearing positions use the spaced form (e.g. `:int` -> `: int`).
Keyword literals in other positions (`:refer`, `:else`, manifest
fields, etc.) are left alone.

See docs/archive/history/spaced-type-annotation-migration-plan.md.

Usage:
  tools/spaced-types-rewrite.py [opts] PATH [PATH...]

Modes (mutually exclusive):
  --write     (default) Rewrite files in place.
  --dry-run   Print unified diff; do not modify.
  --check     Exit 1 if any file would be rewritten.

Other:
  --rewrite-quoted   Also rewrite inside (quote ...) / (quasiquote ...).
  --skip-build-tur   (default) Skip files named build.tur.
  --no-skip-build-tur
"""

from __future__ import annotations

import argparse
import difflib
import os
import sys
from dataclasses import dataclass, field
from typing import Iterable, Optional


# -------------------- Tokenizer --------------------

# Token kinds. Trivia (WS, COMMENT, NEWLINE) are preserved verbatim.
TK_LPAREN     = "LPAREN"      # (
TK_RPAREN     = "RPAREN"      # )
TK_LBRACKET   = "LBRACKET"    # [
TK_RBRACKET   = "RBRACKET"    # ]
TK_LBRACE     = "LBRACE"      # {  (curly-infix)
TK_RBRACE     = "RBRACE"      # }
TK_HASH_LBRACE = "HASH_LBRACE"  # #{
# Dispatch-prefixed braces: #map{ #set{ #vec{ etc — handled below
TK_DISPATCH_LBRACE = "DISPATCH_LBRACE"
TK_QUOTE      = "QUOTE"       # '
TK_QUASIQUOTE = "QUASIQUOTE"  # `
TK_UNQUOTE    = "UNQUOTE"     # ~ or ,
TK_UNQUOTE_SPLICE = "UNQUOTE_SPLICE"  # ,@ or ~@
TK_STRING     = "STRING"      # "..."
TK_CHAR       = "CHAR"        # \c
TK_CODE_FENCE = "CODE_FENCE"  # ```c ... ```
TK_COMMENT    = "COMMENT"     # ; .... \n  (not including trailing newline)
TK_NEWLINE    = "NEWLINE"     # bare \n
TK_WS         = "WS"          # horizontal whitespace
TK_KEYWORD    = "KEYWORD"     # :foo  (does NOT include bare ':')
TK_SYMBOL     = "SYMBOL"      # any non-numeric atom (including :, ->, etc)
TK_NUMBER     = "NUMBER"
TK_HASH_ATOM  = "HASH_ATOM"   # #foo, #'foo, #_, etc; not #{, not #map{, etc
TK_LANG_LINE  = "LANG_LINE"   # #lang sweet-exp (whole line)
TK_SHEBANG    = "SHEBANG"     # #! ... (first-line shebang)


TRIVIA_KINDS = {TK_WS, TK_NEWLINE, TK_COMMENT}


@dataclass
class Tok:
    kind: str
    text: str               # exact source slice
    # value is filled for SYMBOL/KEYWORD/NUMBER -- not used for trivia
    value: Optional[str] = None
    # Used by walker to mark "this fused keyword has been rewritten".
    rewritten: bool = False


def _is_atom_char(c: str) -> bool:
    if c in "()[]{}\"`'~,;\\":
        return False
    if c.isspace():
        return False
    return True


def tokenize(src: str) -> list[Tok]:
    """Tokenize while preserving trivia verbatim.

    Not a full Turmeric reader -- only correct enough to identify the
    type-bearing positions for the codemod. Falls through unrecognised
    bytes as part of the surrounding atom.
    """
    toks: list[Tok] = []
    i = 0
    n = len(src)
    line_start = True  # are we at the start of a line (only whitespace before)?

    while i < n:
        c = src[i]

        # Shebang at file start
        if i == 0 and src.startswith("#!"):
            end = src.find("\n", i)
            if end < 0:
                end = n
            toks.append(Tok(TK_SHEBANG, src[i:end]))
            i = end
            continue

        # #lang directive (only at start of a line)
        if line_start and src.startswith("#lang", i):
            # consume to end of line
            end = src.find("\n", i)
            if end < 0:
                end = n
            toks.append(Tok(TK_LANG_LINE, src[i:end]))
            i = end
            continue

        # Newline
        if c == "\n":
            toks.append(Tok(TK_NEWLINE, "\n"))
            i += 1
            line_start = True
            continue

        # Whitespace (horizontal)
        if c.isspace():
            j = i
            while j < n and src[j] != "\n" and src[j].isspace():
                j += 1
            toks.append(Tok(TK_WS, src[i:j]))
            i = j
            continue

        # Comment
        if c == ";":
            j = i
            while j < n and src[j] != "\n":
                j += 1
            toks.append(Tok(TK_COMMENT, src[i:j]))
            i = j
            continue

        # String literal
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

        # Character literal \c
        if c == "\\":
            # Could also be a path; in Turmeric, \c is a char literal.
            # Consume until whitespace or delimiter.
            j = i + 1
            # at least one char
            if j < n:
                j += 1
            while j < n and _is_atom_char(src[j]):
                j += 1
            toks.append(Tok(TK_CHAR, src[i:j]))
            i = j
            line_start = False
            continue

        # Inline-C / code fences  ```c ... ```
        if src.startswith("```", i):
            j = i + 3
            # find closing ```
            close = src.find("```", j)
            if close < 0:
                close = n
            else:
                close += 3
            toks.append(Tok(TK_CODE_FENCE, src[i:close]))
            i = close
            line_start = False
            continue

        # Brackets / parens
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

        # Hash forms
        if c == "#":
            # #{...} effect/contract set
            if i + 1 < n and src[i + 1] == "{":
                toks.append(Tok(TK_HASH_LBRACE, "#{"))
                i += 2
                line_start = False
                continue
            # #<dispatch>{ ... }  (#map{, #set{, #vec{)
            j = i + 1
            while j < n and _is_atom_char(src[j]) and src[j] != "{":
                j += 1
            if j < n and src[j] == "{":
                toks.append(Tok(TK_DISPATCH_LBRACE, src[i:j + 1]))
                i = j + 1
                line_start = False
                continue
            # #foo, #'foo, #_, etc — generic hash atom
            j = i + 1
            while j < n and _is_atom_char(src[j]):
                j += 1
            toks.append(Tok(TK_HASH_ATOM, src[i:j]))
            i = j
            line_start = False
            continue

        # Quote / quasiquote / unquote
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

        # Keyword :foo (NOT bare ':' on its own)
        if c == ":":
            j = i + 1
            while j < n and _is_atom_char(src[j]):
                j += 1
            if j == i + 1:
                # bare ':' -- treat as a symbol
                toks.append(Tok(TK_SYMBOL, ":", value=":"))
                i = j
                line_start = False
                continue
            text = src[i:j]
            toks.append(Tok(TK_KEYWORD, text, value=text))
            i = j
            line_start = False
            continue

        # Number? (very loose: leading digit or +/-/. followed by digit)
        if c.isdigit() or (c in "+-." and i + 1 < n and src[i + 1].isdigit()):
            j = i
            while j < n and _is_atom_char(src[j]):
                j += 1
            text = src[i:j]
            # Could still be a symbol like `+` or `-` alone; treat numerics
            # only when the whole token parses; otherwise symbol.
            if any(ch.isdigit() for ch in text) and all(
                ch.isdigit() or ch in "+-._eExX0123456789abcdefABCDEF" for ch in text
            ):
                toks.append(Tok(TK_NUMBER, text, value=text))
            else:
                toks.append(Tok(TK_SYMBOL, text, value=text))
            i = j
            line_start = False
            continue

        # Symbol -- catch-all atom
        j = i
        while j < n and _is_atom_char(src[j]):
            j += 1
        if j == i:
            # Unrecognised; emit as a 1-char symbol to avoid infinite loop.
            toks.append(Tok(TK_SYMBOL, src[i:i + 1], value=src[i:i + 1]))
            i += 1
        else:
            text = src[i:j]
            toks.append(Tok(TK_SYMBOL, text, value=text))
            i = j
        line_start = False

    return toks


# -------------------- Form tree --------------------

# Bracket-pair node holds a list of children (Tok or Form).
@dataclass
class Form:
    open: Tok                       # the LPAREN/LBRACKET/etc.
    close: Optional[Tok]            # the closing bracket (None on EOF)
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
    """Parse a flat token list into a tree of Form/Tok nodes."""
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
    """Children, trivia + reader-macro prefixes filtered out, paired with index.

    Returns a list of (orig_index, node) tuples.  We treat QUOTE/QUASIQUOTE/
    UNQUOTE prefix tokens as transparent for indexing -- they immediately
    precede a form that we still want to "see".
    """
    src = form_or_list.children if isinstance(form_or_list, Form) else form_or_list
    out = []
    for i, node in enumerate(src):
        if isinstance(node, Tok):
            if node.kind in TRIVIA_KINDS:
                continue
            if node.kind in (TK_QUOTE, TK_QUASIQUOTE, TK_UNQUOTE, TK_UNQUOTE_SPLICE):
                # Skip: the next code item is the quoted form.
                continue
        out.append((i, node))
    return out


def head_symbol(form: Form) -> Optional[str]:
    """If form is a paren list starting with a symbol, return its name."""
    if not form.is_paren():
        return None
    for i, node in code_children(form):
        if isinstance(node, Tok) and node.kind == TK_SYMBOL:
            return node.value
        return None
    return None


# -------------------- Rewriter --------------------

DEFN_HEADS    = {"defn", "defmacro"}
FN_HEADS      = {"fn"}
LET_HEADS     = {"let", "let*", "loop"}
STRUCT_HEADS  = {"defstruct"}
CLASS_HEADS   = {"defclass", "defprotocol"}  # method sig shape inside body
INSTANCE_HEAD = "definstance"


def _is_caret_modifier(tok) -> bool:
    return (
        isinstance(tok, Tok)
        and tok.kind == TK_SYMBOL
        and tok.value.startswith("^")
    )


def rewrite_keyword_token(t: Tok) -> bool:
    """Convert `:foo` -> `: foo` in-place.  Returns True if changed."""
    if t.kind != TK_KEYWORD or not t.text.startswith(":") or len(t.text) < 2:
        return False
    if t.text.startswith(": "):
        return False  # already spaced (shouldn't happen for KEYWORD kind)
    if t.rewritten:
        return False
    t.text = ": " + t.text[1:]
    t.value = t.text
    t.rewritten = True
    return True


def is_spaced_type_ann(seq: list, i: int) -> bool:
    """Returns True if position i in seq is a bare `:` symbol followed by
    a type form (already-spaced annotation).  Used so we don't double-count.
    """
    node = seq[i]
    return (
        isinstance(node, Tok) and node.kind == TK_SYMBOL and node.value == ":"
    )


def consume_type_form(seq: list, code_items: list, ci_idx: int) -> int:
    """If the next code item is a type form (keyword, symbol, or list),
    return its index into code_items + 1; else return ci_idx unchanged."""
    if ci_idx >= len(code_items):
        return ci_idx
    _, node = code_items[ci_idx]
    if isinstance(node, Tok) and node.kind in (TK_KEYWORD, TK_SYMBOL):
        return ci_idx + 1
    if isinstance(node, Form) and node.is_paren():
        return ci_idx + 1
    return ci_idx


def rewrite_param_vec(vec: Form, stats: "Stats") -> None:
    """Walk a defn/fn param vector and rewrite param type annotations.

    Param shape: [^mod* name (:type | : type)? ...]   plus `& rest :type`.
    """
    items = code_children(vec)
    ci = 0
    while ci < len(items):
        _, node = items[ci]
        # Skip caret modifiers
        if _is_caret_modifier(node):
            ci += 1
            continue
        # `&` rest marker
        if isinstance(node, Tok) and node.kind == TK_SYMBOL and node.value == "&":
            ci += 1
            # rest name
            if ci < len(items):
                ci += 1
            # optional type
            if ci < len(items):
                _, type_node = items[ci]
                if isinstance(type_node, Tok) and type_node.kind == TK_KEYWORD:
                    if rewrite_keyword_token(type_node):
                        stats.rewrites += 1
                    ci += 1
                elif is_spaced_type_ann(vec.children, items[ci][0]):
                    # bare `:` then type form
                    ci += 2  # skip the `:` and the type form
            continue
        # Regular param name
        if isinstance(node, Tok) and node.kind == TK_SYMBOL:
            ci += 1
            # Optional type annotation
            if ci < len(items):
                _, type_node = items[ci]
                if isinstance(type_node, Tok) and type_node.kind == TK_KEYWORD:
                    if rewrite_keyword_token(type_node):
                        stats.rewrites += 1
                    ci += 1
                elif isinstance(type_node, Tok) and type_node.kind == TK_SYMBOL and type_node.value == ":":
                    # already spaced
                    ci += 1
                    # consume the type form
                    if ci < len(items):
                        ci += 1
                elif isinstance(type_node, Form) and (type_node.is_paren() or type_node.is_hash_brace()):
                    # compound type annotation like (forall ...) or #{Effect}
                    ci += 1
            continue
        # Unknown -- skip
        ci += 1


def rewrite_struct_field_vec(vec: Form, stats: "Stats") -> None:
    """Walk a defstruct field vector.  Each field is `name :type`.
    Modifiers like `:copy` appear BEFORE the vec, not inside, so we
    treat every keyword inside as a field type."""
    items = code_children(vec)
    ci = 0
    while ci < len(items):
        _, node = items[ci]
        if isinstance(node, Tok) and node.kind == TK_SYMBOL:
            ci += 1
            if ci < len(items):
                _, type_node = items[ci]
                if isinstance(type_node, Tok) and type_node.kind == TK_KEYWORD:
                    if rewrite_keyword_token(type_node):
                        stats.rewrites += 1
                    ci += 1
                elif isinstance(type_node, Tok) and type_node.kind == TK_SYMBOL and type_node.value == ":":
                    ci += 1
                    if ci < len(items):
                        ci += 1
                elif isinstance(type_node, Form) and (type_node.is_paren() or type_node.is_hash_brace()):
                    ci += 1
            continue
        ci += 1


def rewrite_let_binding_vec(vec: Form, stats: "Stats",
                            quoted_depth: int, rewrite_quoted: bool) -> None:
    """Walk a let binding vector: each binding is `^mod* name [:type | : type] value`.

    A keyword in the type slot is only present sometimes; we must detect
    by lookahead: if `name kw form` parses cleanly as a triple, treat kw
    as a type ann; otherwise treat it as `name kw` where kw is a keyword
    value (rare but valid).
    """
    items = code_children(vec)
    ci = 0
    while ci < len(items):
        _, node = items[ci]
        if _is_caret_modifier(node):
            ci += 1
            continue
        if isinstance(node, Tok) and node.kind == TK_SYMBOL:
            # name
            ci += 1
            # Lookahead: keyword + value-form?
            if ci + 1 < len(items):
                _, maybe_type = items[ci]
                if isinstance(maybe_type, Tok) and maybe_type.kind == TK_KEYWORD:
                    if rewrite_keyword_token(maybe_type):
                        stats.rewrites += 1
                    ci += 1  # consume type
            # Skip spaced `: T` form
            if ci + 1 < len(items):
                _, maybe_sym = items[ci]
                if isinstance(maybe_sym, Tok) and maybe_sym.kind == TK_SYMBOL and maybe_sym.value == ":":
                    ci += 1
                    if ci < len(items):
                        ci += 1
            # value form -- consume AND recurse into it (it may contain
            # nested defn/fn/let with their own annotations).
            if ci < len(items):
                _, value_node = items[ci]
                if isinstance(value_node, Form):
                    walk_form(value_node, stats, quoted_depth, rewrite_quoted)
                ci += 1
            continue
        ci += 1


@dataclass
class Stats:
    rewrites: int = 0


def walk_form(form: Form, stats: Stats, quoted_depth: int = 0,
              rewrite_quoted: bool = False) -> None:
    """Top-down walk.  Identify type-bearing positions and rewrite."""
    if not isinstance(form, Form):
        return

    # Skip the inside of #{...} (effects/contracts) entirely.
    if form.is_hash_brace():
        return
    # Skip the inside of #map{...}, #set{...}, #vec{...} (data literals).
    # Keys inside are runtime keywords, never type annotations.
    if form.is_dispatch_brace():
        return

    # If this is a paren list, inspect its head symbol.
    if form.is_paren():
        head = head_symbol(form)
        if head == "quote" or head == "quasiquote":
            if not rewrite_quoted:
                # do not descend
                return
            quoted_depth += 1
        elif head in DEFN_HEADS or head in FN_HEADS:
            _rewrite_defn_like(form, stats, head, quoted_depth, rewrite_quoted)
            return
        elif head in STRUCT_HEADS:
            _rewrite_defstruct(form, stats, quoted_depth, rewrite_quoted)
            return
        elif head in LET_HEADS:
            _rewrite_let_like(form, stats, head, quoted_depth, rewrite_quoted)
            return
        elif head == INSTANCE_HEAD or head in CLASS_HEADS:
            _rewrite_class_or_instance(form, stats, quoted_depth, rewrite_quoted)
            return

    # Default: recurse into children.
    for child in form.children:
        if isinstance(child, Form):
            walk_form(child, stats, quoted_depth, rewrite_quoted)


def _rewrite_defn_like(form: Form, stats: Stats, head: str,
                       quoted_depth: int, rewrite_quoted: bool) -> None:
    """Rewrite (defn NAME [PARAMS] :RET BODY) and (fn [PARAMS] :RET BODY)."""
    code = code_children(form)
    # code[0] is the head symbol
    idx = 1
    if head in DEFN_HEADS:
        # skip the name (and any type-parameter vec like [A B])
        if idx < len(code):
            idx += 1
        # optional type-parameter vec
        while idx < len(code):
            _, node = code[idx]
            if isinstance(node, Form) and node.is_bracket():
                break  # this is the param vec
            idx += 1
    # Now code[idx] should be the param vector
    param_vec = None
    if idx < len(code):
        _, node = code[idx]
        if isinstance(node, Form) and node.is_bracket():
            param_vec = node
            idx += 1
    if param_vec is not None:
        rewrite_param_vec(param_vec, stats)
    # Return-type slot: zero or more effect/contract braces, then optional :ret
    while idx < len(code):
        _, node = code[idx]
        if isinstance(node, Form) and node.is_hash_brace():
            idx += 1
            continue
        if isinstance(node, Tok) and node.kind == TK_KEYWORD:
            if rewrite_keyword_token(node):
                stats.rewrites += 1
            idx += 1
            break
        if isinstance(node, Tok) and node.kind == TK_SYMBOL and node.value == ":":
            # already spaced
            idx += 1
            if idx < len(code):
                idx += 1
            break
        # No return type
        break
    # Recurse into body forms
    while idx < len(code):
        _, node = code[idx]
        if isinstance(node, Form):
            walk_form(node, stats, quoted_depth, rewrite_quoted)
        idx += 1


def _rewrite_defstruct(form: Form, stats: Stats,
                       quoted_depth: int, rewrite_quoted: bool) -> None:
    """Rewrite (defstruct NAME [MOD...] [FIELDS])."""
    code = code_children(form)
    idx = 1  # skip head
    if idx < len(code):
        idx += 1  # skip name
    # Skip modifiers (`:copy`, etc.) until we hit a bracket vec.
    while idx < len(code):
        _, node = code[idx]
        if isinstance(node, Form) and node.is_bracket():
            rewrite_struct_field_vec(node, stats)
            idx += 1
            break
        idx += 1
    # Recurse into anything after the field vec.
    while idx < len(code):
        _, node = code[idx]
        if isinstance(node, Form):
            walk_form(node, stats, quoted_depth, rewrite_quoted)
        idx += 1


def _rewrite_let_like(form: Form, stats: Stats, head: str,
                      quoted_depth: int, rewrite_quoted: bool) -> None:
    """Rewrite (let [BINDS] body...), (let* [BINDS] body...),
    (loop [BINDS] body...), and named-let (let NAME [BINDS] body...)."""
    code = code_children(form)
    idx = 1  # skip head
    # named-let: optional symbol before the bracket
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
        rewrite_let_binding_vec(bind_vec, stats, quoted_depth, rewrite_quoted)
    # Recurse into body
    while idx < len(code):
        _, node = code[idx]
        if isinstance(node, Form):
            walk_form(node, stats, quoted_depth, rewrite_quoted)
        idx += 1


def _rewrite_class_or_instance(form: Form, stats: Stats,
                               quoted_depth: int, rewrite_quoted: bool) -> None:
    """Method signatures inside defclass/defprotocol/definstance follow the
    defn shape: (method-name [PARAMS] :ret BODY?).  Recurse and rewrite."""
    for child in form.children:
        if isinstance(child, Form) and child.is_paren():
            # Treat each inner paren as a defn-shaped method.
            _rewrite_method_sig(child, stats, quoted_depth, rewrite_quoted)
        elif isinstance(child, Form):
            walk_form(child, stats, quoted_depth, rewrite_quoted)


def _rewrite_method_sig(form: Form, stats: Stats,
                        quoted_depth: int, rewrite_quoted: bool) -> None:
    """(method-name [PARAMS] :ret BODY?) shape."""
    code = code_children(form)
    if not code:
        return
    idx = 1  # method name
    param_vec = None
    if idx < len(code):
        _, node = code[idx]
        if isinstance(node, Form) and node.is_bracket():
            param_vec = node
            idx += 1
    if param_vec is not None:
        rewrite_param_vec(param_vec, stats)
    # return-type slot
    while idx < len(code):
        _, node = code[idx]
        if isinstance(node, Form) and node.is_hash_brace():
            idx += 1
            continue
        if isinstance(node, Tok) and node.kind == TK_KEYWORD:
            if rewrite_keyword_token(node):
                stats.rewrites += 1
            idx += 1
            break
        if isinstance(node, Tok) and node.kind == TK_SYMBOL and node.value == ":":
            idx += 1
            if idx < len(code):
                idx += 1
            break
        break
    while idx < len(code):
        _, node = code[idx]
        if isinstance(node, Form):
            walk_form(node, stats, quoted_depth, rewrite_quoted)
        idx += 1


# -------------------- Serialization --------------------

def serialize_token(t: Tok) -> str:
    return t.text


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


# -------------------- Driver --------------------

def _walk_implicit_seq(nodes: list, stats: Stats,
                       quoted_depth: int, rewrite_quoted: bool) -> None:
    """Walk a flat sequence of nodes looking for sweet-exp top-level forms
    that have no surrounding paren -- e.g. `defn NAME [VEC] :RET` directly
    at file scope.  Matches the same head symbols as walk_form and applies
    the same rewriter logic to the immediately-following items."""
    # Filter to a list of (orig_idx, node) skipping trivia.
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
        if head in DEFN_HEADS or head in FN_HEADS:
            # defn NAME? [VEC] :RET? body...
            ci += 1
            if head in DEFN_HEADS and ci < n:
                # consume name
                _, name_node = code[ci]
                if isinstance(name_node, Tok) and name_node.kind == TK_SYMBOL:
                    ci += 1
                # optional type-parameter vec (skip if it isn't followed
                # by another bracket -- in sweet-exp this is rare)
            # param vec
            if ci < n:
                _, pv = code[ci]
                if isinstance(pv, Form) and pv.is_bracket():
                    rewrite_param_vec(pv, stats)
                    ci += 1
            # return type slot
            while ci < n:
                _, rn = code[ci]
                if isinstance(rn, Form) and rn.is_hash_brace():
                    ci += 1
                    continue
                if isinstance(rn, Tok) and rn.kind == TK_KEYWORD:
                    if rewrite_keyword_token(rn):
                        stats.rewrites += 1
                    ci += 1
                    break
                if isinstance(rn, Tok) and rn.kind == TK_SYMBOL and rn.value == ":":
                    ci += 1
                    if ci < n:
                        ci += 1
                    break
                break
            continue
        if head in STRUCT_HEADS:
            ci += 1
            if ci < n:
                ci += 1  # name
            while ci < n:
                _, nn = code[ci]
                if isinstance(nn, Form) and nn.is_bracket():
                    rewrite_struct_field_vec(nn, stats)
                    ci += 1
                    break
                ci += 1
            continue
        if head in LET_HEADS:
            ci += 1
            if ci < n:
                _, nn = code[ci]
                if isinstance(nn, Tok) and nn.kind == TK_SYMBOL:
                    ci += 1  # named-let name
            if ci < n:
                _, nn = code[ci]
                if isinstance(nn, Form) and nn.is_bracket():
                    rewrite_let_binding_vec(nn, stats, quoted_depth, rewrite_quoted)
                    ci += 1
            continue
        ci += 1


def rewrite_source(src: str, rewrite_quoted: bool = False) -> tuple[str, int]:
    toks = tokenize(src)
    tree = parse(toks)
    stats = Stats()
    # Detect sweet-exp: either a #lang sweet-exp directive or a .tur.sweet
    # extension (caller passes filename via path-based heuristic; here we
    # only have the source).  Cheap check: scan for "#lang sweet-exp".
    is_sweet = any(
        isinstance(t, Tok) and t.kind == TK_LANG_LINE and "sweet-exp" in t.text
        for t in tree
        if isinstance(t, Tok)
    )
    for node in tree:
        if isinstance(node, Form):
            walk_form(node, stats, quoted_depth=0, rewrite_quoted=rewrite_quoted)
    if is_sweet:
        _walk_implicit_seq(tree, stats, quoted_depth=0, rewrite_quoted=rewrite_quoted)
    return serialize(tree), stats.rewrites


def is_skipped(path: str, skip_build_tur: bool) -> bool:
    if skip_build_tur and os.path.basename(path) == "build.tur":
        return True
    return False


def iter_files(paths: Iterable[str]) -> Iterable[str]:
    for p in paths:
        if os.path.isdir(p):
            for root, _, files in os.walk(p):
                for f in files:
                    if f.endswith(".tur") or f.endswith(".tur.sweet"):
                        yield os.path.join(root, f)
        else:
            yield p


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("paths", nargs="+", help="files or directories to process")
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--write", action="store_true", help="rewrite files in place (default)")
    mode.add_argument("--dry-run", action="store_true", help="print unified diff; do not write")
    mode.add_argument("--check", action="store_true", help="exit 1 if any file would be rewritten")
    ap.add_argument("--rewrite-quoted", action="store_true",
                    help="also rewrite inside quote/quasiquote forms")
    ap.add_argument("--no-skip-build-tur", action="store_true",
                    help="do not skip files named build.tur")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    if not (args.dry_run or args.check):
        args.write = True

    skip_build_tur = not args.no_skip_build_tur
    any_changed = False
    total_rewrites = 0
    n_files_changed = 0

    for path in iter_files(args.paths):
        if is_skipped(path, skip_build_tur):
            if args.verbose:
                print(f"skip {path}", file=sys.stderr)
            continue
        try:
            with open(path, "r", encoding="utf-8") as fh:
                src = fh.read()
        except (OSError, UnicodeDecodeError) as e:
            print(f"{path}: read error: {e}", file=sys.stderr)
            return 2
        try:
            new_src, n = rewrite_source(src, rewrite_quoted=args.rewrite_quoted)
        except Exception as e:
            print(f"{path}: rewrite error: {e}", file=sys.stderr)
            return 2
        if new_src != src:
            any_changed = True
            n_files_changed += 1
            total_rewrites += n
            if args.check:
                print(f"{path}: {n} fused annotation(s) need rewrite", file=sys.stderr)
            elif args.dry_run:
                diff = difflib.unified_diff(
                    src.splitlines(keepends=True),
                    new_src.splitlines(keepends=True),
                    fromfile=f"a/{path}", tofile=f"b/{path}",
                )
                sys.stdout.writelines(diff)
            else:  # write
                with open(path, "w", encoding="utf-8") as fh:
                    fh.write(new_src)
                if args.verbose:
                    print(f"rewrote {path} ({n} rewrites)", file=sys.stderr)
        else:
            if args.verbose:
                print(f"clean {path}", file=sys.stderr)

    if args.check:
        if any_changed:
            print(f"{n_files_changed} file(s) need rewrite, {total_rewrites} fused annotation(s) total", file=sys.stderr)
            return 1
        return 0
    if not args.check and any_changed:
        print(f"{n_files_changed} file(s) {'would change' if args.dry_run else 'rewritten'}, {total_rewrites} rewrite(s) total", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
