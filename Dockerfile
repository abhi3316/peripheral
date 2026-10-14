# ==============================================================================
# Peripheral Camera Gateway & Operator Viewer - Linux Container
# Target Environment: Ubuntu 24.04 LTS (Noble Numbat) with GCC 14 (C++26) & Node 22
# ==============================================================================

FROM ubuntu:24.04

# Prevent interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Install modern C++26 build toolchain and system utilities
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    g++-14 \
    gcc-14 \
    cmake \
    ninja-build \
    curl \
    ca-certificates \
    lsof \
    jq \
    git \
    python3 \
    && rm -rf /var/lib/apt/lists/*

# Install Node.js 22 LTS for the camera simulator and React operator viewer
RUN curl -fsSL https://deb.nodesource.com/setup_22.x | bash - && \
    apt-get install -y --no-install-recommends nodejs && \
    rm -rf /var/lib/apt/lists/*

# Set default compiler to GCC 14
ENV CC=gcc-14
ENV CXX=g++-14

WORKDIR /app

# Copy dependency manifests first for optimal Docker layer caching
COPY utils/tools/package*.json ./utils/tools/
COPY utils/viewer/package*.json ./utils/viewer/

# Install Node dependencies
RUN (cd utils/tools && npm ci || npm install) && \
    (cd utils/viewer && npm ci || npm install)

# Copy the entire codebase
COPY . .

# Ensure run.sh is executable
RUN chmod +x run.sh

# Build the C++26 Gateway Server and run all unit tests to guarantee build integrity
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build -j && \
    ctest --test-dir build --output-on-failure

# Build the React Operator Viewer production bundle
RUN (cd utils/viewer && npm run build)

# Expose all operational and simulator ports:
# 8080: Gateway REST API & Operator Viewer (production bundle)
# 5173: Operator Viewer (Vite dev server)
# 7001/7002: Camera 0 (Control / Video)
# 7011/7012: Camera 1 (Control / Video)
# 7000/7010: Camera 0 / 1 Simulator Admin Stats
# 7100/7101: Cloud Storage API / Admin Stats
EXPOSE 8080 5173 7001 7002 7011 7012 7000 7010 7100 7101

# Default entrypoint launches the complete stack
ENTRYPOINT ["./run.sh"]
