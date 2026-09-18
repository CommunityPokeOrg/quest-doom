# android-openxr-loader

Standalone Android build of the **official Khronos OpenXR loader**
(`libopenxr_loader.so`) compiled **from source** — not the prebuilt Maven AAR.
Produces per-ABI `.so` files, an Android-library **AAR** carrier, and a
**smoke-test APK** that loads the library and initializes it on-device.

## Provenance

| | |
|---|---|
| Upstream | KhronosGroup/OpenXR-SDK-Source |
| Tag | `release-1.1.63` |
| Tarball | `https://github.com/KhronosGroup/OpenXR-SDK-Source/archive/refs/tags/release-1.1.63.tar.gz` |
| Tarball sha256 | `a3b97a36f11abe256a7ea1668a0a468aac9b738e94bea6b468f0ae31ad537a46` |
| License | Apache-2.0 (Khronos) |

`fetch-source.sh` downloads the pinned tarball, verifies the checksum, and
extracts to `openxr-sdk-source/` (not committed to git).

## Requirements

- Android SDK, platform android-34
- Android NDK `27.2.12479018` (`sdkmanager "ndk;27.2.12479018" "cmake;3.22.1"`)
- JDK 17, Gradle 8.9 (wrapper included)
- `ANDROID_HOME` (or `ANDROID_NDK`) env var

## Build

```bash
cd android-openxr-loader
./fetch-source.sh      # one-time: download + verify upstream source
./build-loader.sh      # cmake+ndk for arm64-v8a and armeabi-v7a, strip, stage
./gradlew assemble     # builds the AAR and the smoke-test APK
```

## Outputs

| Artifact | Path |
|---|---|
| Loader .so (arm64-v8a) | `build/arm64-v8a/src/loader/libopenxr_loader.so` |
| Loader .so (armeabi-v7a) | `build/armeabi-v7a/src/loader/libopenxr_loader.so` |
| Stripped copies | `{openxr-loader,smoketest}/src/main/jniLibs/<abi>/` |
| Library AAR | `openxr-loader/build/outputs/aar/openxr-loader-release.aar` |
| Smoke-test APK | `smoketest/build/outputs/apk/debug/smoketest-debug.apk` |

`./verify.sh` prints `file`, `readelf -d` DT_NEEDED, and exported-symbol
checks (`nm -D`) for each staged `.so`.

## Using the loader in your app

- The AAR ships `jni/<abi>/libopenxr_loader.so`; add it to your app's
  `jniLibs` or depend on the AAR.
- **Android quirk:** `xrInitializeLoaderKHR` is *not* an exported symbol —
  fetch it via `xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", …)`
  and call it with `XrLoaderInitInfoAndroidKHR{vm, clazz}` **before** any other
  `xr*` call (see `smoketest/src/main/cpp/smoke.c` for a working example).
- OpenXR headers for consumers are at `openxr-sdk-source/include/openxr/`
  after `fetch-source.sh`.

## Smoke test on-device

```bash
adb install smoketest/build/outputs/apk/debug/smoketest-debug.apk
adb shell am start org.communitypoke.openxrsmoke/android.app.NativeActivity
adb logcat -s OpenXrSmoke
```

Expected: `dlopen ok`, `xrInitializeLoaderKHR ok`, then `SMOKE PASS`.
`XR_ERROR_RUNTIME_UNAVAILABLE` from extension enumeration is expected on
devices without an OpenXR runtime.

## Build configuration

NDK CMake toolchain, `ANDROID_PLATFORM=android-24`, `Release`,
`BUILD_API_LAYERS=OFF`, `BUILD_TESTS=OFF`, `BUILD_CONFORMANCE_TESTS=OFF`,
`BUILD_SDK_TESTS=OFF`, exception handling at upstream default (ON — disabling
it breaks the vendored jsoncpp under `-Werror=undef`).

## Limitations

- Debug builds of the loader are not stripped by `build-loader.sh` variants —
  release `.so`s here are `llvm-strip --strip-unneeded`.
- Loader defaults to system-wide API layer scanning via the package manager;
  dynamic API-layer loading beyond the runtime is untested.
- Min API is 24; 32-bit armeabi-v7a is included for completeness — most XR
  runtimes (incl. Quest) are arm64-only.
- This builds only the loader; API layers and tests are intentionally off.
