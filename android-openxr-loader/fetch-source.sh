#!/usr/bin/env bash
# Fetch the official Khronos OpenXR-SDK-Source at a pinned release tag and
# verify its checksum. Produces ./openxr-sdk-source/.
set -euo pipefail
cd "$(dirname "$0")"

VERSION="release-1.1.63"
TAG_SHA="2b99fec95e9cdf352c1a98e9cb23bf4def1cf8e6"   # annotated tag object
COMMIT_SHA="2b99fec95e9cdf352c1a98e9cb23bf4def1cf8e6^{commit}"
# tarball sha256 of github codeload archive for the tag (recorded 2026-09-18)
TARBALL_SHA256="a3b97a36f11abe256a7ea1668a0a468aac9b738e94bea6b468f0ae31ad537a46"
URL="https://github.com/KhronosGroup/OpenXR-SDK-Source/archive/refs/tags/${VERSION}.tar.gz"

if [ -d openxr-sdk-source/src/loader ]; then
    echo "openxr-sdk-source already present — skipping fetch"
    exit 0
fi

echo "Downloading ${URL}"
curl -fSL -o openxr-sdk.tar.gz "$URL"
echo "$TARBALL_SHA256  openxr-sdk.tar.gz" | sha256sum -c -
rm -rf openxr-sdk-source
mkdir openxr-sdk-source
tar xzf openxr-sdk.tar.gz --strip-components=1 -C openxr-sdk-source
rm openxr-sdk.tar.gz
echo "OpenXR-SDK-Source ${VERSION} extracted to openxr-sdk-source/"
