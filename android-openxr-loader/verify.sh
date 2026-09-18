#!/usr/bin/env bash
# Verify built loader .so files: architecture, DT_NEEDED, exported symbols.
set -euo pipefail
cd "$(dirname "$0")"

for ABI in arm64-v8a armeabi-v7a; do
    SO="openxr-loader/src/main/jniLibs/$ABI/libopenxr_loader.so"
    echo "===== $ABI ====="
    file "$SO"
    echo "-- DT_NEEDED --"
    readelf -d "$SO" | grep NEEDED
    echo "-- exported xr* symbols --"
    nm -D "$SO" | grep -c " T xr"
    nm -D "$SO" | grep -E " T (xrCreateInstance|xrGetInstanceProcAddr)$"
    if nm -D "$SO" | grep -q xrInitializeLoaderKHR; then
        echo "!! xrInitializeLoaderKHR unexpectedly exported"
    else
        echo "ok: xrInitializeLoaderKHR not exported (fetch it via xrGetInstanceProcAddr)"
    fi
    sha256sum "$SO"
done
