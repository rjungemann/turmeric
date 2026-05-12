CC      ?= cc
WARN    := -Wall -Wextra -Werror -Wno-unused-parameter -std=c99 -pedantic
DEBUG   := -Og -g -fsanitize=address,undefined -DTUR_DEBUG=1
RELEASE := -O2 -DNDEBUG

CFLAGS  ?= $(WARN) $(DEBUG)
LDFLAGS ?=

SRCS := $(wildcard src/*.c)
OBJS := $(patsubst src/%.c,build/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

BIN := build/tur

.PHONY: all release debug tsan test test-tsan clean

all: debug

debug: CFLAGS := $(WARN) $(DEBUG)
debug: $(BIN)

release: CFLAGS := $(WARN) $(RELEASE)
release: $(BIN)

# T19: ThreadSanitizer build — compile tur itself with -fsanitize=thread so
# that fixtures compiled by the test runner also receive the flag via the
# TUR_CC_FLAGS env var (set in tests/run.sh when TUR_TSAN=1).
tsan: CFLAGS := $(WARN) -O1 -g -fsanitize=thread
tsan: $(BIN)

$(BIN): $(OBJS) | build
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

build:
	@mkdir -p build

test: $(BIN)
	@bash tests/run.sh
	@bash tests/run-cli.sh
	@bash tests/check-span-unknown.sh

# T19: Run the full test suite with ThreadSanitizer enabled.
# Rebuilds tur with -fsanitize=thread and sets TUR_TSAN=1 for the runner.
test-tsan: tsan
	@TUR_TSAN=1 bash tests/run.sh
	@TUR_TSAN=1 bash tests/run-cli.sh
	@TUR_TSAN=1 bash tests/check-span-unknown.sh

clean:
	rm -rf build tests/out
	find tests/fixtures -name 'actual.*' -delete
	find tests/cli -name 'actual.*' -delete

-include $(DEPS)
