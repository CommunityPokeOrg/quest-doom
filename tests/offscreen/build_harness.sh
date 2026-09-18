#!/usr/bin/env bash
# Build the offscreen render-verification harness (desktop Mesa EGL/GLES3).
# Usage: tests/offscreen/build_harness.sh  ->  /tmp/qd-harness/harness
set -euo pipefail
cd "$(dirname "$0")/../.."   # repo root
OUT=/tmp/qd-harness
mkdir -p "$OUT"

CPP=app/src/main/cpp
DOOM_SRCS=$(ls $CPP/doomgeneric/*.c | grep -v 'doomgeneric_[a-z]*\.c$')

gcc -O1 -g -o "$OUT/harness" \
    -DNORMALUNIX -DLINUX -D_DEFAULT_SOURCE \
    -w -fno-strict-aliasing \
    -I$CPP -I$CPP/doomgeneric \
    tests/offscreen/harness.c $CPP/quest/quest_stubs.c \
    $CPP/quest/gl_renderer.c $CPP/quest/gl_world.c \
    $DOOM_SRCS \
    -lEGL -lGLESv2 -lm -lpthread
echo "built $OUT/harness"
