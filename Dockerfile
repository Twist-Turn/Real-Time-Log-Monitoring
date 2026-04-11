# ─── Stage 1: Builder ───────────────────────────────────────────────────────
FROM gcc:13 AS builder

RUN apt-get update && apt-get install -y \
    cmake ninja-build \
    libncurses5-dev \
    libcurl4-openssl-dev \
    libssl-dev \
    git \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

# Copy build files (layer-cache friendly: CMakeLists first, then sources)
COPY CMakeLists.txt .
COPY src/        src/
COPY config/     config/
COPY tests/      tests/
COPY scripts/    scripts/

# Configure (FetchContent downloads happen here; cached in BuildKit layer)
RUN cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS="-O2"

# Build main binary and all tests
RUN cmake --build build --parallel "$(nproc)"

# Run tests (fail the build if any test fails)
RUN cd build && ctest --output-on-failure

# ─── Stage 2: Runtime ───────────────────────────────────────────────────────
FROM ubuntu:22.04

# Install only runtime dependencies
RUN apt-get update && apt-get install -y \
    libncurses6 \
    libcurl4 \
    libssl3 \
    curl \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy binary
COPY --from=builder /build/build/logmonitor /app/logmonitor

# Copy default config
COPY config/ /app/config/

# Create directories for persistent data and watched logs
RUN mkdir -p /app/data /app/logs /tmp/watched_logs

# Default environment variables (override at runtime)
ENV API_KEY=changeme
ENV MAX_MEMORY_MB=512

# Expose ports:
#   5514 — TCP log ingestion (legacy NetworkIngester)
#   9090 — HTTP API (cpp-httplib HttpServer)
EXPOSE 5514/tcp
EXPOSE 9090/tcp

# Entrypoint: pass config path as first argument
CMD ["/app/logmonitor", "/app/config/config.json"]
