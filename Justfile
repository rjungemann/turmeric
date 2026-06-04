# Default: configure (if needed) then debug build
default: build

# ---------------------------------------------------------------------------
# Build targets
# ---------------------------------------------------------------------------

build: debug

debug:
    @[ -f build/CMakeCache.txt ] || just configure
    cmake --build build -j --config Debug

release:
    @[ -f build/CMakeCache.txt ] || just configure
    cmake --build build -j --config Release

tsan:
    @[ -f build/CMakeCache.txt ] || just configure
    cmake --build build -j --config TSan

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

configure:
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_POLICY_VERSION_MINIMUM=3.5

reconfigure:
    rm -rf build
    just configure

# ---------------------------------------------------------------------------
# Test targets
# ---------------------------------------------------------------------------

test: build doctest
    # NOTE: Run with a 5-minute timeout for now, due to some minor hang issues
    timeout 300 ctest --output-on-failure --progress --test-dir build

# Run stdlib doctests (generate + run).
doctest: build
    python3 tools/doctest.py stdlib/ --out tests/doctest-generated/
    bash tools/run-doctests.sh

# Run only the compiled-fixture test suite (tur tests).
test-tur: build
    bash tests/run.sh

# Run the interpreter fixture test suite (turi tests).
test-turi: build
    bash tests/run-turi.sh

test-tsan: tsan
    TUR_TSAN=1 ctest --output-on-failure --test-dir build

# Run production smoke tests against live turmeric-lang.com + try.turmeric-lang.com.
# Requires: npm install run from the web/ directory (just web-deps).
smoke:
    cd web && npm run test:prod

# ---------------------------------------------------------------------------
# Cleanup
# ---------------------------------------------------------------------------

clean:
    rm -rf build build-release build-tsan build-wasm tests/out
    find tests/fixtures -name 'actual.*' -delete
    find tests/cli -name 'actual.*' -delete

clean-test:
    rm -rf tests/out
    find tests/fixtures -name 'actual.*' -delete
    find tests/cli -name 'actual.*' -delete

# ---------------------------------------------------------------------------
# Utility
# ---------------------------------------------------------------------------

repl: build
    ./build/tur repl

run file:
    ./build/tur run {{file}}

build-file file:
    ./build/tur build {{file}}

# Phase C2: build a user program with contracts stripped (release builds).
# Contract predicates are dropped at elaboration; see docs/guides/compiler-flags-guide.md.
release-stripped file:
    ./build/tur --no-contracts build {{file}} -O2

emit-c file:
    ./build/tur emit-c {{file}}

# ---------------------------------------------------------------------------
# Games / Examples (CMake targets — require Raylib)
# ---------------------------------------------------------------------------

# Enable the examples subtree (Raylib) and build the snake target.
# On a cold build, prefer installing raylib via your package manager first
# (e.g. `brew install raylib`) so CPM does not need to compile it from source.
configure-examples:
    cmake -S . -B build -DTUR_EXAMPLES=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5

snake: configure-examples
    cmake --build build --target snake

minikanren: configure-examples
    cmake --build build --target minikanren

roguelike: configure-examples
    cmake --build build --target roguelike

games: snake minikanren roguelike

run-snake: snake
    ./build/examples/snake/snake

run-minikanren: minikanren
    ./build/examples/minikanren/minikanren

run-roguelike: roguelike
    ./build/examples/roguelike-raylib/roguelike

guestbook: configure-examples
    cmake --build build --target guestbook

run-guestbook: guestbook
    ./build/examples/guestbook/guestbook

# ---------------------------------------------------------------------------
# Script examples (interpreted via `tur run`)
# ---------------------------------------------------------------------------

run-cellular-automata: build
    ./build/tur run examples/cellular-automata.tur

# ---------------------------------------------------------------------------
# Full rebuild
# ---------------------------------------------------------------------------

rebuild: clean configure build

# ---------------------------------------------------------------------------
# Documentation
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Documentation
# ---------------------------------------------------------------------------

# Generate HTML API docs from stdlib ;;; docstrings.
# Also emits stdlib/docstrings.tur for the runtime (doc name) lookup,
# and web/public/doc-names.json for the web REPL search bar.
# Spice symbols are folded into doc-names.json via --extra-json so the web
# search bar surfaces stdlib + spices in a single list.
docs: guides spices
    python3 tools/gendocs.py stdlib/ --out docs/html/api/ --emit-tur stdlib/docstrings.tur --emit-json web/public/doc-names.json --extra-json docs/html/spices/doc-names-spices.json

# Render markdown guides to HTML pages (served at /docs/html/guides/).
guides:
    python3 tools/genguides.py docs/guides/ --out docs/html/guides/

# Generate per-spice doc pages from the sibling ../turmeric-spices/ checkout.
# Also emits docs/html/spices/doc-names-spices.json which gendocs merges into
# web/public/doc-names.json on the next step.
spices:
    python3 tools/genspices.py --out docs/html/spices/ --emit-json docs/html/spices/doc-names-spices.json


# Check that every turmeric+sweet-exp toggle pair in the guides is valid.
check-guides:
    python3 tools/check-guide-pairs.py docs/guides/

# Strict check for spice READMEs: every `turmeric block must have an adjacent
# `sweet-exp sibling (or be marked `turmeric no-check`).
check-spices:
    python3 tools/check-guide-pairs.py --spices

# Run both guide and spice README checks.
check-docs: check-guides check-spices

# ---------------------------------------------------------------------------
# WebAssembly & Web REPL targets
# ---------------------------------------------------------------------------

# Configure with WASM support
configure-wasm:
    cmake -S . -B build-wasm -DTUR_WASM=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5

alias build-wasm := wasm

# Build WASM module (requires Emscripten).
# Runs `just docs` first so stdlib/docstrings.tur is up-to-date.
wasm: docs configure-wasm
    cmake --build build-wasm -j --target tur_wasm

# Set up web dependencies (Monaco Editor, etc.)
web-deps:
    cd web && npm install

# Build web bundle (requires Vite)
web: wasm web-deps
    cd web && npm run build

# Deploy to GitHub Pages
deploy-web: web
    # cd web/dist && git init && git add . && git commit -m "Deploy to GitHub Pages"
    # git push -f git@github.com:turmeric-lang/turmeric.git main:gh-pages
    cd web && npm run deploy

# Run web dev server
web-dev: web-deps
    cd web && npm run dev

# Clean WASM build
clean-wasm:
    rm -rf build-wasm

# ---------------------------------------------------------------------------
# Release tagging
# ---------------------------------------------------------------------------

# Bump the patch version (0.2.1 -> 0.2.2) and push a release tag.
bump-patch:
    #!/usr/bin/env bash
    set -euo pipefail
    OLD=$(cat VERSION)
    IFS='.' read -r MAJOR MINOR PATCH <<< "$OLD"
    NEW="$MAJOR.$MINOR.$((PATCH + 1))"
    echo "$NEW" > VERSION
    sed -i.bak "s/TURMERIC_VERSION \"$OLD\"/TURMERIC_VERSION \"$NEW\"/" src/web/wasm_glue.h
    rm -f src/web/wasm_glue.h.bak
    git add VERSION src/web/wasm_glue.h
    git commit -m "chore: bump version to v$NEW"
    git tag -a "v$NEW" -m "Release v$NEW"
    git push origin HEAD "v$NEW"

# Bump the minor version (0.2.1 -> 0.3.0) and push a release tag.
bump-minor:
    #!/usr/bin/env bash
    set -euo pipefail
    OLD=$(cat VERSION)
    IFS='.' read -r MAJOR MINOR PATCH <<< "$OLD"
    NEW="$MAJOR.$((MINOR + 1)).0"
    echo "$NEW" > VERSION
    sed -i.bak "s/TURMERIC_VERSION \"$OLD\"/TURMERIC_VERSION \"$NEW\"/" src/web/wasm_glue.h
    rm -f src/web/wasm_glue.h.bak
    git add VERSION src/web/wasm_glue.h
    git commit -m "chore: bump version to v$NEW"
    git tag -a "v$NEW" -m "Release v$NEW"
    git push origin HEAD "v$NEW"

# Bump the major version (0.2.1 -> 1.0.0) and push a release tag.
bump-major:
    #!/usr/bin/env bash
    set -euo pipefail
    OLD=$(cat VERSION)
    IFS='.' read -r MAJOR MINOR PATCH <<< "$OLD"
    NEW="$((MAJOR + 1)).0.0"
    echo "$NEW" > VERSION
    sed -i.bak "s/TURMERIC_VERSION \"$OLD\"/TURMERIC_VERSION \"$NEW\"/" src/web/wasm_glue.h
    rm -f src/web/wasm_glue.h.bak
    git add VERSION src/web/wasm_glue.h
    git commit -m "chore: bump version to v$NEW"
    git tag -a "v$NEW" -m "Release v$NEW"
    git push origin HEAD "v$NEW"

# ---------------------------------------------------------------------------
# Performance comparison
# ---------------------------------------------------------------------------

PERF := "performance-comparison"

# Build a release binary (needed by perf tasks that invoke Turmeric benchmarks).
perf-build:
    cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
    cmake --build build-rel -j --config Release

# Validate correctness of all benchmark implementations (small inputs).
perf-validate:
    cd {{PERF}} && python3 scripts/validate_correctness.py

# Regenerate golden reference output files (run once after adding benchmarks).
perf-golden:
    cd {{PERF}} && python3 scripts/validate_correctness.py --golden

# Print language runtime versions and hardware details.
perf-env:
    cd {{PERF}} && bash scripts/check_environment.sh

# Run all benchmark categories at small input size.
perf-run:
    cd {{PERF}} && bash scripts/run_all.sh

# Run all benchmark categories at the given size: small | medium | large.
perf-run-size size:
    cd {{PERF}} && bash scripts/run_all.sh all {{size}}

# Run a single category (e.g. `just perf-run-cat numerical`).
perf-run-cat category:
    cd {{PERF}} && bash scripts/run_all.sh {{category}}

# Run a single category at a given size.
perf-run-cat-size category size:
    cd {{PERF}} && bash scripts/run_all.sh {{category}} {{size}}

# Aggregate and normalize raw results (trimmed mean, C-relative speedups).
perf-aggregate:
    cd {{PERF}} && python3 scripts/aggregate_results.py

# Check reproducibility: compute CV for each benchmark group, flag CV > 10%.
perf-reproducibility:
    cd {{PERF}} && python3 scripts/check_reproducibility.py

# Identify outlier runs and compute speedup rankings.
perf-analyze:
    cd {{PERF}} && python3 scripts/analyze_results.py --print-summary

# Generate analysis markdown documents (comparison, by-category, by-language, conclusions).
perf-docs:
    cd {{PERF}} && python3 scripts/generate_analysis.py

# Generate ASCII charts and analysis/report.html.
perf-viz:
    cd {{PERF}} && python3 scripts/visualize_results.py

# Open the HTML report in the default browser.
perf-open:
    open {{PERF}}/analysis/report.html

# Full pipeline: run benchmarks → aggregate → analyze → generate docs → open report.
perf: perf-run perf-aggregate perf-analyze perf-docs perf-viz perf-open
