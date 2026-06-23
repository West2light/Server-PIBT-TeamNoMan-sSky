#!/bin/bash

set -e

if ! command -v cmake >/dev/null 2>&1; then
	echo "Error: cmake is not installed or not on PATH." >&2
	echo "Install cmake 3.16+ and Boost development packages, then rerun ./compile.sh." >&2
	exit 1
fi

mkdir -p build

# build exec for cpp
# -j2: khớp 2 vCPU (compile C++ là CPU-bound; nhiều job hơn core không nhanh hơn
#      mà chỉ tăng RAM → OOM). Chỉ build target cần thiết (server + smoke test),
#      KHÔNG build 'lifelong'/'epibt_smoke_test' nặng → tránh treo ở 92%.
cmake -B build ./ -DPYTHON=false -DCMAKE_BUILD_TYPE=Release
cmake --build build --target pibt_tcp_server dual_width_smoke_test -j2

# G-1: dual-width smoke test — fails build if static delta regression is reintroduced
echo "[compile.sh] Running dual-width smoke test..."
./build/dual_width_smoke_test


# build exec for python

# cmake -B build ./ -DPYTHON=true -DCMAKE_BUILD_TYPE=Release
# cmake --build build --target pibt_tcp_server -j2
