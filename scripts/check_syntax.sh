#!/usr/bin/env bash
# Syntax-check the D3D9 backend against real MinGW Windows headers.
#
# The build target is Windows and this work is developed in a Linux container, so
# "it compiles" used to be a claim nobody could make from a checkout - full builds
# happen only on the owner's machine and in dusklight-ao's CI. This makes the cheap
# half checkable here: real <d3d9.h>, real <windows.h>, real repo headers, real fmt
# and absl, -fsyntax-only.
#
# WHAT IT IS NOT. It does not link, does not run, and shims Dawn/WebGPU and xxHash
# (see scripts/syntax-harness/). Passing it means the code parses and type-checks
# against the target's headers - signatures agree, overloads resolve, members exist.
# It is not "builds" and it is certainly not "works". docs/dx9/README.md
# §"Verification vocabulary" is the authority on which word to use.
#
# Three configurations are checked; see CONFIGS below for what each one is for. The
# d3d9-off pass is not optional: every dx9 entry point has a no-op stub behind #else in
# dx9.hpp, and a signature change that misses the stub fails only in the d3d9-off build -
# which is exactly the build nobody runs locally.
#
# Setup, once:
#   sudo apt-get install -y g++-mingw-w64-x86-64 libfmt-dev libabsl-dev
#   scripts/check_syntax.sh
set -uo pipefail

cd "$(dirname "$0")/.."

CXX=${CXX_MINGW:-x86_64-w64-mingw32-g++}
if ! command -v "$CXX" >/dev/null 2>&1; then
  echo "check_syntax: $CXX not found." >&2
  echo "  sudo apt-get install -y g++-mingw-w64-x86-64 libfmt-dev libabsl-dev" >&2
  exit 2
fi

HARNESS="scripts/syntax-harness"
if [ ! -d "$HARNESS/webgpu" ] || [ ! -f "$HARNESS/fmt-base-shim.h" ]; then
  echo "check_syntax: $HARNESS is missing - it should be checked in beside this script" >&2
  exit 2
fi

# Assemble the include tree in a temp dir rather than adding -I/usr/include, which
# would drag glibc's headers into a MinGW compile and fail before reaching our code.
# Real absl and fmt, the checked-in shims layered on top.
INC=$(mktemp -d)
trap 'rm -rf "$INC"' EXIT

for lib in absl fmt; do
  if [ ! -d "/usr/include/$lib" ]; then
    echo "check_syntax: /usr/include/$lib not found - apt-get install libfmt-dev libabsl-dev" >&2
    exit 2
  fi
done

ln -s /usr/include/absl "$INC/absl"
cp -r /usr/include/fmt "$INC/fmt"
cp "$HARNESS/fmt-base-shim.h" "$INC/fmt/base.h"
cp -r "$HARNESS/webgpu" "$INC/webgpu"
cp -r "$HARNESS/tracy" "$INC/tracy"
cp -r "$HARNESS/SDL3" "$INC/SDL3"
cp "$HARNESS/xxhash.h" "$INC/xxhash.h"

UNITS=(
  lib/dx9/dx9_backend.cpp
  lib/dx9/dx9_tev.cpp
  lib/dx9/dx9_draw.cpp
  lib/dx9/dx9_texture.cpp
  lib/dx9/dx9_vertex.cpp
  lib/gx/command_processor.cpp
  lib/dolphin/gx/GXAurora.cpp
)
# NOT checked, and worth knowing which: lib/gx/gx.cpp and lib/gfx/common.cpp reach Dawn
# proper, which is shimmed only as far as the headers above it mention. Changes confined
# to those two are covered only by dusklight-ao's Windows CI.

# TARGET_PC is not optional - without it GXTexObj is sized for the GameCube and every
# translation unit fails a static_assert that has nothing to do with the change at hand.
FLAGS=(-std=c++20 -fsyntax-only -DTARGET_PC -Iinclude -Ilib -I. -I"$INC")

fails=0
checked=0
missing=0

# Three configurations, each covering something the others cannot:
#
#   d3d9=on            the backend proper.
#   d3d9=off           the no-op stubs behind #else in dx9.hpp - see the note above.
#   d3d9=on,NDEBUG     the release side of the AURORA_GFX_DEBUG_GROUPS blocks. That macro
#                      is defined by include/aurora/gfx.h only when NDEBUG is NOT set, so
#                      without this pass every #if defined(AURORA_GFX_DEBUG_GROUPS) body
#                      is checked and none of its #else is - which is the half a release
#                      build compiles. Added 2026-08-16, after a document
#                      claimed harness coverage the harness did not have.
#
# Only the first config reports a missing unit, so a listed-but-absent file is named once.
CONFIGS=(
  "on|-DAURORA_ENABLE_D3D9=1"
  "off|"
  "on,NDEBUG|-DAURORA_ENABLE_D3D9=1 -DNDEBUG"
)

for entry in "${CONFIGS[@]}"; do
  cfg=${entry%%|*}
  read -r -a cfgflags <<< "${entry#*|}"

  for unit in "${UNITS[@]}"; do
    if [ ! -f "$unit" ]; then
      [ "$cfg" = "on" ] && { echo "check_syntax: listed unit not found: $unit" >&2; missing=$((missing + 1)); }
      continue
    fi
    checked=$((checked + 1))
    if ! out=$("$CXX" "${FLAGS[@]}" "${cfgflags[@]}" "$unit" 2>&1); then
      fails=$((fails + 1))
      echo "FAIL [d3d9=$cfg] $unit"
      echo "$out" | grep -E "error:" | sed 's/\[with .*//' | sort -u | head -12
      echo
    fi
  done
done

if [ "$fails" -ne 0 ] || [ "$missing" -ne 0 ]; then
  echo "check_syntax: $fails of $checked translation units failed ($missing listed but absent)" >&2
  exit 1
fi

echo "check_syntax: $checked translation units syntax-checked (d3d9 on, d3d9 off, d3d9 on + NDEBUG)"
