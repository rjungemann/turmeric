# Default: configure (if needed) then debug build
default: build

# ---------------------------------------------------------------------------
# Build targets
# ---------------------------------------------------------------------------

build: debug

debug:
    cmake --build build -j --config Debug

release:
    cmake --build build -j --config Release

tsan:
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

test: build
    ctest --output-on-failure --test-dir build

test-tsan: tsan
    TUR_TSAN=1 ctest --output-on-failure --test-dir build

# ---------------------------------------------------------------------------
# Cleanup
# ---------------------------------------------------------------------------

clean:
    cmake --build build --target clean
    rm -rf build tests/out
    find tests/fixtures -name 'actual.*' -delete
    find tests/cli -name 'actual.*' -delete

clean-test:
    rm -rf tests/out
    find tests/fixtures -name 'actual.*' -delete
    find tests/cli -name 'actual.*' -delete

# ---------------------------------------------------------------------------
# Utility
# ---------------------------------------------------------------------------

run file:
    ./build/tur run {{file}}

build-file file:
    ./build/tur build {{file}}

emit-c file:
    ./build/tur emit-c {{file}}

# ---------------------------------------------------------------------------
# Games
# ---------------------------------------------------------------------------

games:
    cmake --build build --target snake

run-snake: games
    ./build/examples/snake/snake

# ---------------------------------------------------------------------------
# Full rebuild
# ---------------------------------------------------------------------------

rebuild: clean configure build
