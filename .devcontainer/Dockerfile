FROM mcr.microsoft.com/devcontainers/cpp:1-ubuntu-22.04

# libedit: REPL line editing (matches CI: apt-get install libedit-dev)
# python3: doc generation (gendocs.py, genguides.py, doctest.py, ...)
# curl/gnupg: needed to add NodeSource repo below
RUN apt-get update && apt-get install -y --no-install-recommends \
    libedit-dev \
    python3 \
    python3-pip \
    curl \
    gnupg \
    && rm -rf /var/lib/apt/lists/*

# Node.js 20 LTS (Vite 6 requires Node 18+)
RUN curl -fsSL https://deb.nodesource.com/setup_20.x | bash - \
    && apt-get install -y nodejs \
    && rm -rf /var/lib/apt/lists/*

# just — https://just.systems
RUN curl --proto '=https' --tlsv1.2 -sSf https://just.systems/install.sh \
    | bash -s -- --to /usr/local/bin
