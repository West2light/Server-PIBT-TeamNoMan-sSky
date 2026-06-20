# ============================================================
# Dockerfile – Hugging Face Space (Docker SDK)
# Project: Server-PIBT-TeamNoMan-sSky
# Binary chính: pibt_tcp_server (Unity TCP server)
# API layer:    FastAPI proxy (HTTP 7860 → TCP 7777 nội bộ)
# ============================================================

# ── Stage 1: Builder ─────────────────────────────────────────
FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

# Cài build dependencies
# Theo unity_server_guide.md §2: cmake, C++17, Boost (program_options, system,
# filesystem, log, log_setup)
RUN apt-get update && apt-get install -y --no-install-recommends \
        cmake \
        build-essential \
        libboost-all-dev \
        python3-pip \
        python3-dev \
    && rm -rf /var/lib/apt/lists/*

# Copy mã nguồn
WORKDIR /MAPF/codes
COPY . .

# ── Build CHỈ target pibt_tcp_server (theo unity_server_guide.md §4) ──
# KHÔNG dùng ./compile.sh (build toàn bộ, quá chậm trên Free CPU)
# Chỉ build target cần thiết để tránh timeout
RUN mkdir -p build \
    && cmake -B build -S . \
        -DPYTHON=false \
        -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --target pibt_tcp_server --parallel 2

# ── Stage 2: Runtime ─────────────────────────────────────────
FROM ubuntu:22.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive \
    PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1

# Runtime libs: Boost đầy đủ (bao gồm log_setup mà pibt_tcp_server cần)
# Dùng libboost-all-dev ở runtime để chắc chắn không thiếu .so nào
RUN apt-get update && apt-get install -y --no-install-recommends \
        libboost-all-dev \
        python3 \
        python3-pip \
    && rm -rf /var/lib/apt/lists/*

# Copy pibt_tcp_server binary từ builder
COPY --from=builder /MAPF/codes/build/pibt_tcp_server /usr/local/bin/pibt_tcp_server

# Copy project files (example_problems, config.json, python scripts)
WORKDIR /app
COPY --from=builder /MAPF/codes /app

# Cài Python deps cho FastAPI proxy layer
RUN python3 -m pip install --upgrade --no-cache-dir pip \
    && python3 -m pip install --no-cache-dir \
        fastapi \
        uvicorn[standard]

# HF Spaces: expose port 7860 (HTTP) - bắt buộc
EXPOSE 7860
# pibt_tcp_server nội bộ chạy trên 7777 (không expose ra ngoài)

# Entrypoint: uvicorn chạy FastAPI proxy, FastAPI sẽ tự spawn pibt_tcp_server
CMD ["uvicorn", "app:app", "--host", "0.0.0.0", "--port", "7860"]
