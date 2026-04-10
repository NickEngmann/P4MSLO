# P4MSLO Docker CI/CD Image
# Supports both ESP-IDF cross-compilation and host-based testing
#
# Build: docker build -t p4mslo-ci .
# Test:  docker run --rm p4mslo-ci test
# Build firmware: docker run --rm -v $(pwd)/output:/output p4mslo-ci build

FROM ubuntu:24.04 AS base

ENV DEBIAN_FRONTEND=noninteractive
ENV LC_ALL=C.UTF-8
ENV LANG=C.UTF-8

RUN apt-get update && apt-get install -y --no-install-recommends \
    git wget curl ca-certificates \
    cmake ninja-build \
    gcc g++ \
    python3 python3-pip python3-venv \
    libffi-dev libssl-dev \
    && rm -rf /var/lib/apt/lists/*

# ---- Stage: Host Tests (fast, no ESP-IDF needed) ----
FROM base AS test-runner

WORKDIR /workspace
COPY . /workspace/

RUN cd /workspace/test && \
    mkdir -p build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Debug && \
    make -j$(nproc)

CMD ["bash", "-c", "cd /workspace/test/build && for t in test_*; do [ -x \"$t\" ] && ./$t; done"]

# ---- Stage: ESP-IDF Build (full cross-compilation) ----
FROM espressif/idf:v5.5.1 AS idf-builder

WORKDIR /workspace
COPY . /workspace/

# Build factory demo firmware for ESP32-P4
RUN . $IDF_PATH/export.sh && \
    cd /workspace/factory_demo && \
    idf.py set-target esp32p4 && \
    idf.py build 2>&1 | tail -20

# ---- Stage: CI Output ----
FROM base AS ci

COPY --from=test-runner /workspace/test/build /test-binaries
COPY --from=idf-builder /workspace/factory_demo/build/factory_demo.bin /firmware/ 2>/dev/null || true
COPY --from=idf-builder /workspace/factory_demo/build/factory_demo.elf /firmware/ 2>/dev/null || true
COPY --from=idf-builder /workspace/factory_demo/build/bootloader/bootloader.bin /firmware/ 2>/dev/null || true
COPY --from=idf-builder /workspace/factory_demo/build/partition_table/partition-table.bin /firmware/ 2>/dev/null || true

COPY test/run_tests.sh /run_tests.sh
RUN chmod +x /run_tests.sh

ENTRYPOINT ["/bin/bash", "-c"]
CMD ["cd /test-binaries && for t in test_*; do [ -x \"$t\" ] && ./$t; done"]
