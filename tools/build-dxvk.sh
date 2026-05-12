#!/usr/bin/env bash
#
# Build DXVK from the pinned third_party/dxvk submodule and deploy the
# 32-bit DLLs (d3d8.dll, d3d9.dll) to Game/ for Wilbur.
#
# Wilbur.exe is 32-bit, so we only build the i686 variant. We invoke
# DXVK's upstream package-release.sh with --32-only --dev-build so the
# build directory is preserved (faster incremental rebuilds during
# development) and no tarball is produced.
#
# Toolchain prerequisites — see docs/dxvk-build.md for installation
# details. Briefly:
#   - MSYS2 (Windows) with MINGW32 environment + mingw-w64-i686 packages
#     mingw-w64-i686-gcc, mingw-w64-i686-meson, mingw-w64-i686-ninja,
#     mingw-w64-i686-glslang
#   - OR Debian/Ubuntu with mingw-w64 (i686 cross), meson, ninja-build,
#     glslang-tools, plus posix-thread alternatives for the i686-w64-mingw32-{gcc,g++}
#
# Usage:
#   tools/build-dxvk.sh           # builds + deploys to Game/
#   tools/build-dxvk.sh --no-deploy # only builds, doesn't copy to Game/
#   tools/build-dxvk.sh --clean   # wipes the build directory first
#
# Run from MSYS2 MinGW32 shell on Windows, or from WSL/Linux with the
# i686 mingw-w64 toolchain installed.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DXVK_SRC="${ROOT_DIR}/third_party/dxvk"
BUILD_OUT="${ROOT_DIR}/third_party/dxvk-build"
GAME_DIR="${ROOT_DIR}/Game"

DXVK_VERSION="$(cd "${DXVK_SRC}" && git describe --tags --always 2>/dev/null || echo "local")"

opt_clean=0
opt_no_deploy=0
for arg in "$@"; do
  case "${arg}" in
    --clean)     opt_clean=1 ;;
    --no-deploy) opt_no_deploy=1 ;;
    -h|--help)
      sed -n '1,40p' "${BASH_SOURCE[0]}"
      exit 0
      ;;
    *)
      echo "build-dxvk: unknown argument: ${arg}" >&2
      exit 2
      ;;
  esac
done

# ---- Prereq checks ---------------------------------------------------------

require_cmd() {
  local cmd="$1"
  if ! command -v "${cmd}" >/dev/null 2>&1; then
    echo "build-dxvk: required command not found: ${cmd}" >&2
    echo "  See docs/dxvk-build.md for toolchain install instructions." >&2
    exit 3
  fi
}

require_cmd meson
require_cmd ninja
require_cmd i686-w64-mingw32-gcc
require_cmd i686-w64-mingw32-g++
require_cmd glslang || require_cmd glslangValidator
require_cmd git

if [ ! -f "${DXVK_SRC}/meson.build" ]; then
  echo "build-dxvk: DXVK submodule not initialized at ${DXVK_SRC}" >&2
  echo "  Run: git submodule update --init --recursive" >&2
  exit 4
fi

# Verify mingw-w64 i686 has posix threads (DXVK requirement).
mingw_thread_model="$(i686-w64-mingw32-gcc -v 2>&1 | grep -oE 'Thread model: [a-z0-9]+' | head -1)"
case "${mingw_thread_model}" in
  *posix*) ;;
  *)
    echo "build-dxvk: i686-w64-mingw32-gcc thread model is '${mingw_thread_model}'." >&2
    echo "  DXVK requires POSIX threads (cv_status, etc.). On Debian/Ubuntu:" >&2
    echo "    update-alternatives --config i686-w64-mingw32-gcc" >&2
    echo "    update-alternatives --config i686-w64-mingw32-g++" >&2
    echo "  and select the posix variants." >&2
    exit 5
    ;;
esac

# ---- Build -----------------------------------------------------------------

if [ ${opt_clean} -eq 1 ] && [ -d "${BUILD_OUT}" ]; then
  echo "build-dxvk: cleaning ${BUILD_OUT}"
  rm -rf "${BUILD_OUT}"
fi

mkdir -p "${BUILD_OUT}"

echo "build-dxvk: source     = ${DXVK_SRC} (DXVK ${DXVK_VERSION})"
echo "build-dxvk: build dir  = ${BUILD_OUT}"
echo "build-dxvk: arch       = i686 (32-bit) only — Wilbur.exe is 32-bit"
echo "build-dxvk: deploy     = $([ ${opt_no_deploy} -eq 1 ] && echo no || echo yes)"
echo

# DXVK upstream's package-release.sh handles meson setup + ninja install
# for us. --dev-build keeps the build dir (faster rebuilds), --no-package
# skips the tarball.
"${DXVK_SRC}/package-release.sh" "${DXVK_VERSION}" "${BUILD_OUT}" --32-only --dev-build

DLL_DIR="${BUILD_OUT}/dxvk-${DXVK_VERSION}/x32"
if [ ! -f "${DLL_DIR}/d3d8.dll" ] || [ ! -f "${DLL_DIR}/d3d9.dll" ]; then
  echo "build-dxvk: build completed but DLLs missing in ${DLL_DIR}" >&2
  echo "build-dxvk: contents:" >&2
  ls -la "${DLL_DIR}" >&2 || true
  exit 6
fi

echo
echo "build-dxvk: build OK"
echo "  d3d8.dll: $(stat -c%s "${DLL_DIR}/d3d8.dll" 2>/dev/null || echo unknown) bytes"
echo "  d3d9.dll: $(stat -c%s "${DLL_DIR}/d3d9.dll" 2>/dev/null || echo unknown) bytes"

# ---- Deploy ----------------------------------------------------------------

if [ ${opt_no_deploy} -eq 1 ]; then
  echo
  echo "build-dxvk: --no-deploy specified, leaving DLLs at:"
  echo "  ${DLL_DIR}"
  exit 0
fi

if [ ! -d "${GAME_DIR}" ]; then
  echo "build-dxvk: Game directory not found at ${GAME_DIR}" >&2
  echo "  Build succeeded but cannot deploy. Use --no-deploy to skip." >&2
  exit 7
fi

# Refuse to overwrite a non-DXVK d3d{8,9}.dll without confirmation —
# protects the user's dxwrapper-built DLLs until they're ready to
# switch over (Phase C).
for dll in d3d8.dll d3d9.dll; do
  target="${GAME_DIR}/${dll}"
  if [ -f "${target}" ]; then
    # DXVK DLLs contain the "DXVK" marker string in their .rdata. If the
    # current file lacks it, it's likely the dxwrapper-built copy and we
    # warn before overwriting.
    if ! grep -aq "DXVK" "${target}" 2>/dev/null; then
      echo "build-dxvk: ${target} appears NOT to be a DXVK build (no 'DXVK' marker)."
      echo "  This is likely the dxwrapper-built copy. Overwriting is what we want"
      echo "  for Phase C, but if you're testing in parallel you may want to back it up."
      echo "  Press Enter to overwrite, Ctrl+C to abort."
      read -r _
    fi
  fi
  cp -v "${DLL_DIR}/${dll}" "${target}"
done

echo
echo "build-dxvk: deployed to ${GAME_DIR}"
echo "  d3d8.dll, d3d9.dll → DXVK ${DXVK_VERSION}"
echo
echo "Next steps (after Phase C is complete):"
echo "  - Remove Game/dxwrapper.dll and Game/dxwrapper.ini"
echo "  - Launch Wilbur.exe and verify the DXVK HUD with: DXVK_HUD=1 in env"
