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

.PHONY: all release debug test clean

all: debug

debug: CFLAGS := $(WARN) $(DEBUG)
debug: $(BIN)

release: CFLAGS := $(WARN) $(RELEASE)
release: $(BIN)

$(BIN): $(OBJS) | build
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

build:
	@mkdir -p build

test: $(BIN)
	@bash tests/run.sh
	@bash tests/run-cli.sh

clean:
	rm -rf build tests/out
	find tests/fixtures -name 'actual.*' -delete
	find tests/cli -name 'actual.*' -delete

-include $(DEPS)
