#!/usr/bin/env bash
# RM3 R5 graduation item 2 (regions-plan.md): the static escape walk admits
# shapes ONE AT A TIME, each pinned here with its retire/rewind verdict AND its
# value read back after the pop.  This is the second batch, after the
# scalar-field ADT result that region-scope-adt-result pins.
#
# ADMITTED (must REWIND -- tur_region_pop_checked):
#   r-struct  a `defstruct` with scalar fields.  Every defstruct lowers to a
#             record ADT (structdef-retirement DS-D), so it reaches the same
#             TY_ADT arm as a `defdata` and its `[x : int y : float]` fields are
#             scalar keywords the form-based widening admits.  Nothing special
#             was built for it; this pins that it stays admitted.
#   r-cstr    a variant holding a `:cstr`.  A C string is never region memory
#             (no path routes a char buffer through the generation allocator),
#             and `cstr` is on the scalar-keyword list.
#   r-enum    a variant holding a field-less by-value enum.  The form `Color` is
#             NOT a scalar keyword, so it is admitted the other way: the field's
#             `full_type` is recorded (record_full in resolve_ctor_field) and the
#             walk descends into Color, whose every ctor is field-less and so
#             reaches nothing.  That is the inference: nothing else could have
#             let it through.
#
# REFUSED (must RETIRE -- tur_region_pop):
#   r-heap    a variant HOLDING a `:heap` node.  The result reaches region
#             memory, so the walk refuses on `is_heap`.  It is DEFINED so its
#             retire is counted, but deliberately NOT called from main: a record
#             with a `:heap` field returned through a CAPTURING generic `[A]` fat-thunk
#             bracket reads its INT field back as garbage -- with the flag off
#             too, so it is not the region's doing.  Filed as
#             docs/reported/capturing-thunk-returning-heap-field-record-garbles-int.md.
#             Running it would bake a nondeterministic pointer into this file.
#
# The counts are the ratchet: a widening that admits `r-heap` shows up as a
# changed number here AND, because a retired region reclaims nothing, could
# only ever turn into a wrong answer in the run lines below -- which is the SR4
# lesson this whole family asserts values for.  region-scope-adt-result keeps
# the mutual-recursion negative; this file does not duplicate it.
set -u
TMP="$1"
TUR="${TUR:-./build/tur}"

cat > "$TMP/in.tur" <<'EOF'
(defdata Link :heap (Link [v : int nxt : int]))
(defn build [n : int acc : int] : int
  (if (<= n 0) acc (build (- n 1) (:: (Link n acc) :int))))
(defn chain-sum [c : int] : int
  ```c
  struct { int64_t v; int64_t nxt; } *p = (void *)(intptr_t)c;
  int64_t acc = 0; while (p) { acc += p->v; p = (void *)(intptr_t)p->nxt; } return acc;
  ```)

(defstruct P2 [x : int y : float])
(defn r-struct [n : int] : P2
  (with-region (fn [] (make-struct P2 :x (chain-sum (build n 0)) :y 1.5))))

(defdata Named (N :int :cstr))
(defn r-cstr [n : int] : Named
  (with-region (fn [] (N (chain-sum (build n 0)) "hi"))))

(defdata Color (Red) (Green) (Blue))
(defdata Tag (T :int :Color))
(defn r-enum [n : int] : Tag
  (with-region (fn [] (T (chain-sum (build n 0)) (Green)))))

(defdata HoldsLink (HL :int :Link))
(defn r-heap [n : int] : HoldsLink
  (with-region (fn [] (HL (chain-sum (build n 0)) (Link n 0)))))

(defn main [] : int
  (match (r-struct 4) (P2 a b) (println a))   ;; 10
  (match (r-cstr 4)   (N a s)  (println a))   ;; 10
  (match (r-enum 4)   (T a c)  (println a))   ;; 10
  0)
EOF

"$TUR" --enable=regions emit-c "$TMP/in.tur" 2>/dev/null > "$TMP/out.c"

if grep -q 'tur_region_push()' "$TMP/out.c"; then
    echo "bracket: opened"
else
    echo "bracket: MISSING"
fi
retire=$(grep -v '^extern' "$TMP/out.c" | grep -c 'tur_region_pop(')
rewind=$(grep -v '^extern' "$TMP/out.c" | grep -c 'tur_region_pop_checked(')
echo "retire=$retire rewind=$rewind"
echo "regions off: $("$TUR" run "$TMP/in.tur" 2>/dev/null | tr '\n' ' ')"
echo "regions on:  $("$TUR" --enable=regions run "$TMP/in.tur" 2>/dev/null | tr '\n' ' ')"
exit 0
