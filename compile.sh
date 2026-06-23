#!/bin/bash

set -e

if ! command -v cmake >/dev/null 2>&1; then
	echo "Error: cmake is not installed or not on PATH." >&2
	echo "Install cmake 3.16+ and Boost development packages, then rerun ./compile.sh." >&2
	exit 1
fi

mkdir -p build

# build exec for cpp

cmake -B build ./ -DPYTHON=false -DCMAKE_BUILD_TYPE=Release
make -C build -j

# G-1: dual-width smoke test — fails build if static delta regression is reintroduced
echo "[compile.sh] Running dual-width smoke test..."
./build/dual_width_smoke_test


# build exec for python

# cmake -B build ./ -DPYTHON=true -DCMAKE_BUILD_TYPE=Release
# make -C build -j
