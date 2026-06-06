# syntax=docker/dockerfile:1
# Build the Turmeric compiler from the local source tree and produce a
# self-contained image with `tur` on PATH.
#
# Build:
#   docker build -t turmeric .
#
# REPL:
#   docker run --rm -it turmeric
#
# Run a file:
#   docker run --rm -v "$(pwd)":/workspace turmeric tur run /workspace/hello.tur
#
# Interpret a file (no C compiler needed):
#   docker run --rm -v "$(pwd)":/workspace turmeric tur --interpret /workspace/hello.tur

# ── stage 1: build ──────────────────────────────────────────────────────
FROM ubuntu:22.04 AS build

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake libedit-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    && cmake --build build -j"$(nproc)"

# ── stage 2: runtime ────────────────────────────────────────────────────
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# libedit2: REPL line editing at runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
    libedit2 ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Keep the binary, stdlib, and C runtime sources together under /turmeric so
# that autolink markers (e.g. "src/runtime/hamt.c") resolve from the same root.
COPY --from=build /src/build/tur   /turmeric/tur
COPY --from=build /src/stdlib      /turmeric/stdlib
COPY --from=build /src/src/runtime /turmeric/src/runtime

ENV TUR_STDLIB_DIR=/turmeric/stdlib

# Wrapper: change into /turmeric before invoking tur so that the relative
# src/runtime/* paths used by `tur run`/`tur build` resolve correctly.
RUN printf '#!/bin/sh\ncd /turmeric && exec /turmeric/tur "$@"\n' \
    > /usr/local/bin/tur && chmod +x /usr/local/bin/tur

WORKDIR /workspace
CMD ["tur", "repl"]
