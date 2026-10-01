# A rank-2 class method's result came back as the wrong word

**Severity: high.** A silent wrong answer, and a segfault or invalid C on
the aggregate variant:

```turmeric
(defclass Cf [a] (cf [x : a] : a))
(definstance Cf [float] (cf [x : float] : float {x + 1.25}))
(defn r2f [a] [(Cf a)] [x : a] : a (cf x))
(defn use-f [l (forall [a] [(Cf a)] (-> a a)) v : float] : float (l v))
(use-f r2f 7.1)     ; compiled: 3.95253e-323   interpreter: 8.35
```

Found 2026-10-01 by the type fuzzer's new `x_rank2_class` crossing
(`tests/type-fuzz-src.py`, seed 5150): 11 `BUG_invalid_c` and 3
`BUG_value_conversion` in 400 cases. This is the result-side twin of
`dict-classvar-float-param-value-converted.md`, which fixed the parameter
side. **RESOLVED 2026-10-01.**

## Mechanism

A class method whose declared result is the class variable (`(cf [x : a] : a)`)
is called through the dictionary at a site that cannot know the instance, so
the site reads the result as the int64 word. Each slot held the raw instance
method, though, so:

- `double` / `float` results came back in a float register while the caller
  read an integer one (the denormals above). On the concrete end, the dict
  clone returned the double's value converted, not its bits.
- A by-value ADT result (`tur_adt_W`, more than 8 bytes) was returned
  by value into an `int64_t` function. With one instance that was invalid C
  (`incompatible types when returning type 'tur_adt_W'`). With two instances
  the representative's cast compiled, and the caller dereferenced a
  struct's first word as a box pointer and segfaulted.
- A zero-parameter method (`(dflt [] : a)`) had no wrapper at all, so the
  fix's struct field type and the slot fill disagreed
  (`__dictwrap_Default_default_hyof_bool` undeclared).

## Fix

- `dict_slot_result_is_word_scalar` (`emit_stmt.c`) answers whether the
  class result is the class variable while the instance returns a
  non-word scalar, a pointer, or a by-value aggregate. Such a slot holds a
  `__dictwrap_*` that returns the word: the bits of a float, a pointer through
  `intptr_t`, a narrow int widened, an aggregate boxed (malloc plus
  `TUR_REGION_NOTE_WORDS`). The struct field and the dispatch site in
  `emit_call_name` (`emit_core.c`) are spelled to match. A method with no
  parameters now gets a wrapper when its result needs one.
- The dict clone's return passes the int64 temp through as it is, without
  re-casting it as a pointer (`emit_fns.c`).
- A clone whose emitted return is the word but whose tail is a concrete
  aggregate or float (the mixed spec `__inst_Rw_rw_W__spec__int64_t_tur_adt_W`)
  boxes the aggregate or returns the float's bits (`carrier_return_concrete_stmt`).
  This is gated on the emitted signature (`fn_emitted_ret_is_word`), not on
  `current_fn_ret_ctype`, because an `Option` return also reads as int64 there
  and must not be boxed twice.
- In the arg chain, a carrier word passed into a parameter emitted as
  `double` / `float` / `tur_adt_*` is converted from bits, or dereferenced from
  its box.

## Verified

`tests/fixtures/rank2-class-result-word-converted` runs compiled and
`--interpret` with 0 float-lint findings. It covers float, float32, bool,
cstr, int16, an ADT with one instance and with two, direct calls, and a
zero-parameter method. On the baseline compiler the file is invalid C. With the
ADT lines removed, the float and float32 rank-2 lines print `3.95253e-323` and
`9.809089e-45`.
