# Plan: Regex literals, regex union API, and named reader string macros

> **Status:** Proposed
> **Last Updated:** 2026-05-26
> **Type:** Reader + stdlib/re

---

## Overview

This plan bundles three closely related enhancements:

1. Add Racket-style regex literal shorthand using hash-dispatch string form:
   #rx"^\\d+$"
2. Add stdlib helpers in stdlib/re.tur for unioning multiple regex patterns.
3. Extend hash-dispatch reader macros to support named string macros in the
   general form:
   #some-name"some-body"

These features fit together naturally: #rx"..." is a concrete built-in
instance of a generalized named string macro reader path, and the regex union
helpers provide the composition primitive needed by regex-heavy macro users.

---

## Goals

1. Make regex literals concise and readable in source files.
2. Provide a standard way to combine many patterns into one regex.
3. Generalize reader string dispatch so language and spice authors can define
   #name"body" forms without adding new hardcoded reader branches.
4. Preserve current reader behavior for existing forms.

## Non-goals

1. Replace existing prefix macros (quote, quasiquote, unquote, and friends).
2. Add compile-time regex execution or validation beyond optional syntax checks.
3. Introduce non-standard regex engines in this phase (stay on POSIX regex.h).
4. Add arbitrary custom delimiters for string macros in this phase.

---

## Current state

1. stdlib/re.tur provides compile, match, find, and replace APIs, but no helper
   for constructing alternation patterns from multiple inputs.
2. The reader already supports several hardcoded hash-dispatch forms and user
   reader-macro planning work exists in docs/archive/reader-macros-plan.md.
3. There is currently no first-class #name"body" dispatch path.

---

## Proposed language surface

### 1) Regex literal shorthand

Reader form:

#rx"^\\d+$"

First-cut expansion target:

(re/compile "^\\d+$")

Notes:

1. String escaping follows normal Turmeric string literal rules before it
   reaches regex compilation.
2. This first cut keeps behavior explicit and consistent with current re/compile
   lifetime rules.

### 2) Regex union helpers in stdlib/re.tur

Add two APIs:

1. re/union-patterns [patterns :int] :cstr
2. re/compile-union [patterns :int] :int

Semantics:

1. patterns is a cons list of cstr pattern strings.
2. re/union-patterns wraps each element in grouping parentheses and joins with
   vertical bar.
3. re/compile-union is a convenience wrapper around re/union-patterns followed
   by re/compile.
4. Empty input returns 0 (error sentinel) in phase one to avoid defining an
   implicit match-nothing regex in POSIX ERE.

Example output shape for three patterns:

([A-Za-z]+)|([0-9]+)|(foo-bar)

### 3) General named reader string macros

Reader form:

#some-name"some-body"

General expansion model:

1. Parse identifier after # using existing symbol-name character policy.
2. If immediately followed by quote, read one standard string literal body.
3. Dispatch to a registry entry for the name with body kind string.

Initial built-in registration:

1. Register rx as a built-in named string macro.
2. Expansion template for rx is equivalent to (re/compile <string-body>).

---

## Reader implementation plan

### Phase RS1: Reader string-macro dispatch in parser

1. Extend hash-dispatch parse logic to detect #<ident>"...".
2. Add dispatch body kind string to the reader macro registry record.
3. Keep existing # forms unchanged and keep existing diagnostics for unknown
   prefixes.

Candidate files:

1. src/compiler/reader.c
2. src/compiler/reader_macros.c
3. src/compiler/reader_macros.h

### Phase RS2: Built-in rx registration

1. Register rx in built-in macro table (non-overridable by user macros).
2. Expand to the same form as manual re/compile usage.
3. Ensure diagnostics mention #rx explicitly on malformed string literals.

### Phase RS3: Public registration support for string body kind

1. Extend reader-macro definition surface to declare body kind string.
2. Keep strict collision checks with reserved built-ins.
3. Document load-order semantics (macros must be available before read).

---

## Stdlib implementation plan for regex union

### Phase RU1: Pattern union constructor

1. Implement re/union-patterns in stdlib/re.tur with inline C.
2. Validate every list element is non-null cstr.
3. Allocate one output cstr and return ownership to caller.

### Phase RU2: Compile convenience helper

1. Implement re/compile-union in stdlib/re.tur.
2. Build union pattern, compile it, free temporary pattern buffer.
3. Return compiled handle or 0 on failure.

### Phase RU3: Documentation and examples

1. Add docstrings with the project standard format.
2. Include examples with digit-only and alpha-only alternation.

---

## Diagnostics

New or updated reader errors:

1. unknown reader string macro '#name'
2. reader string macro '#name' expects string body
3. reserved reader macro '#rx' cannot be redefined

Regex union function errors:

1. re/union-patterns returns 0 if list is empty
2. re/union-patterns returns 0 if any element is null
3. re/compile-union returns 0 if construction or compilation fails

---

## Tests

### Reader tests

1. #rx"^\\d+$" expands and matches numeric input.
2. Unknown #name"..." reports the new targeted diagnostic.
3. Existing hash-dispatch forms still parse unchanged.

### Stdlib regex tests

1. Union of two patterns matches either branch.
2. Union of three patterns preserves left-to-right alternation behavior.
3. Empty-list union returns 0.

Potential fixture locations:

1. tests/fixtures/reader/
2. tests/fixtures/stdlib/re/

---

## Risks and mitigations

1. Risk: Reader ambiguity with existing # forms.
   Mitigation: Gate string-macro path on exact #<ident>" prefix and keep
   existing branches in precedence-tested order.

2. Risk: Capture-group renumbering surprises in unioned patterns.
   Mitigation: Document grouping behavior and preserve deterministic wrapper
   shape per branch.

3. Risk: Handle lifetime confusion for #rx literals.
   Mitigation: Document that #rx currently expands to re/compile semantics and
   must follow current re/free expectations.

---

## Acceptance criteria

1. #rx"..." works as a reader shorthand and compiles to existing regex runtime.
2. #name"body" dispatch is supported by the reader macro system.
3. stdlib/re.tur includes list-based regex union helpers.
4. Existing reader forms and current regex APIs remain backward compatible.
5. Test suite coverage includes parser, diagnostics, and regex union behavior.
