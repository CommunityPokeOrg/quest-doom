#!/usr/bin/env bash
# Build libopenxr_loader.so from official source for arm64-v8a + armeabi-v7a
# using the Android NDK CMake toolchain, then stage stripped copies into the
# library module and smoke-test app jniLibs directories.
set -euo pipefail
cd "$(dirname "$0")"

ANDROID_NDK="${ANDROID_NDK:-${ANDROID_HOME:?set ANDROID_HOME or ANDROID_NDK}/ndk/27.2.12479018}"
TOOLCHAIN="$ANDROID_NDK/build/cmake/android.toolchain.cmake"
STRIP="$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"
ABIS="arm64-v8a armeabi-v7a"

[ -f "$TOOLCHAIN" ] || { echo "NDK toolchain not found: $TOOLCHAIN"; exit 1; }
[ -d openxr-sdk-source ] || ./fetch-source.sh

for ABI in $ABIS; do
    echo "=== configuring $ABI ==="
    cmake -GNinja -S openxr-sdk-source -B "build/$ABI" \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
        -DANDROID_ABI="$ABI" \
        -DANDROID_PLATFORM=android-24 \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_API_LAYERS=OFF -DBUILD_TESTS=OFF \
        -DBUILD_CONFORMANCE_TESTS=OFF -DBUILD_SDK_TESTS=OFF
    echo "=== building $ABI ==="
    cmake --build "build/$ABI" --target openxr_loader
    SRC="build/$ABI/src/loader/libopenxr_loader.so"
    for dst in openxr-loader/src/main/jniLibs smoketest/src/main/jniLibs; do
        mkdir -p "$dst/$ABI"
        "$STRIP" --strip-unneeded "$SRC" -o "$dst/$ABI/libopenxr_loader.so"
        echo "  -> $dst/$ABI/libopenxr_loader.so"
    done
done

echo
echo "Stripped outputs:"
find . -name libopenxr_loader.so -not -path "./build/*" -not -path "./openxr-sdk-source/*" \
    -exec sha256sum {} \;
