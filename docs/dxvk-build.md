# Building DXVK from source

This project ships DXVK as a source submodule (`third_party/dxvk`,
pinned to a specific upstream tag) and builds it from source rather
than dropping in a prebuilt release. See
[research/findings/dxvk-migration-plan-2026-05-08.md](../research/findings/dxvk-migration-plan-2026-05-08.md)
for why.

This document covers the toolchain install and build commands. The
actual build is automated via [tools/build-dxvk.sh](../tools/build-dxvk.sh).

## Toolchain prerequisites

DXVK uses **mingw-w64 (POSIX-thread variant)** + **meson** + **ninja**
+ **glslang** to cross-compile Windows DLLs. Wilbur.exe is 32-bit so
we only need the i686 cross toolchain.

### Option 1: MSYS2 (Windows native build)

Recommended for users who want to build entirely on Windows without WSL.

1. Install MSYS2: <https://www.msys2.org/>
2. Open the **"MSYS2 MINGW32"** shell (NOT the regular MSYS2 shell —
   we want the i686 environment).
3. Update package database:
   ```
   pacman -Syu
   ```
4. Install the build dependencies:
   ```
   pacman -S --needed git \
     mingw-w64-i686-gcc \
     mingw-w64-i686-meson \
     mingw-w64-i686-ninja \
     mingw-w64-i686-glslang
   ```
5. Verify:
   ```
   i686-w64-mingw32-gcc -v 2>&1 | grep -i thread
   ```
   Should print `Thread model: posix`. If `win32`, you have the wrong
   variant — uninstall and reinstall.

6. Run the build from the MSYS2 MINGW32 shell:
   ```
   cd /d/Projects/Programming/MeetTheRobinsons
   tools/build-dxvk.sh
   ```

### Option 2: WSL or native Linux

If you already have a Linux/WSL environment, this is faster than MSYS2.

#### Debian / Ubuntu

```
sudo apt install --no-install-recommends \
    git meson ninja-build \
    mingw-w64 mingw-w64-i686-dev \
    glslang-tools
```

Then ensure the i686 mingw-w64 GCC uses POSIX threads (some distros
default to win32):
```
sudo update-alternatives --config i686-w64-mingw32-gcc
sudo update-alternatives --config i686-w64-mingw32-g++
```

Pick the `i686-w64-mingw32-gcc-posix` variant from each menu.

Verify:
```
i686-w64-mingw32-gcc -v 2>&1 | grep -i thread
```

Should print `Thread model: posix`.

#### Fedora

```
sudo dnf install meson ninja-build mingw32-gcc mingw32-gcc-c++ \
                 glslang
```

Fedora ships with POSIX threads by default — no alternatives switch
needed.

#### Arch

```
sudo pacman -S meson ninja mingw-w64-gcc glslang
```

## Building

Once the toolchain is installed, just run:

```
tools/build-dxvk.sh
```

This:
1. Verifies the toolchain.
2. Invokes DXVK's upstream `package-release.sh` with `--32-only --dev-build`.
3. Outputs `d3d8.dll` and `d3d9.dll` to `third_party/dxvk-build/dxvk-<version>/x32/`.
4. Copies them to `Game/d3d8.dll` and `Game/d3d9.dll`.

For incremental rebuilds (during DXVK source modifications):

```
cd third_party/dxvk-build/dxvk-<version>/build.32
ninja install
# then re-run tools/build-dxvk.sh to redeploy, or copy DLLs manually
```

Build flags:
- `--no-deploy` — only build, don't copy to Game/.
- `--clean` — wipe the build directory first (full rebuild).

## Verifying the DXVK build at runtime

After deploy, launch Wilbur.exe with `DXVK_HUD=1` in the environment:

```
set DXVK_HUD=full
Game\Wilbur.exe
```

You should see DXVK's overlay in the top-left corner showing GPU
device, framerate, and Vulkan API info. If the HUD doesn't appear,
either DXVK isn't loaded, the game crashed early, or it fell back to
the system d3d9.dll.

## Updating the pinned DXVK version

The submodule is pinned to a specific upstream tag for reproducibility.
To bump the version:

```
cd third_party/dxvk
git fetch --tags
git checkout vX.Y.Z
git submodule update --init --recursive
cd ../..
git add third_party/dxvk
git commit -m "Bump DXVK to vX.Y.Z"
```

Then rebuild + verify with the regression suite.

## Troubleshooting

### "error: 'std::cv_status' has not been declared"

Your mingw-w64 GCC is using `win32` threads instead of `posix`. Switch
via `update-alternatives` (Ubuntu/Debian) or reinstall the `posix`
variant package (MSYS2: `mingw-w64-i686-gcc` already includes posix
support).

### Meson can't find glslang

Install `glslang` (MSYS2) or `glslang-tools` (Debian/Ubuntu). Verify
the binary is on PATH: `which glslang` or `which glslangValidator`.

### Build runs but DLLs don't load in Wilbur

1. Confirm 32-bit: the DLLs in `Game/` must be PE32 (i386), not PE32+.
   Check with `file d3d8.dll` (Linux/MSYS2) or PowerShell:
   ```
   $bytes = [byte[]](Get-Content -Encoding Byte Game/d3d8.dll | Select -First 0x100)
   # PE machine type is at e_lfanew + 4 + 0; 0x14C = i386, 0x8664 = x86_64
   ```
2. Check Wilbur.exe's import directory still points at `d3d8.dll` (it
   does on a stock install — the launcher is what matters).
3. Confirm Vulkan runtime is present on the system (Vulkan ICD layer).
   Test with `vulkaninfo` from the Vulkan SDK.
4. Check `Game/d3d8.log` and `d3d9.log` (DXVK writes these on Windows
   by default) for the actual failure reason.

### Game crashes early on DXVK

This is usually one of:
- **Missing Vulkan layer**: install the latest GPU driver. NVIDIA
  500-series+, AMD RX 400+, Intel Arc all support Vulkan 1.3.
- **DXVK incompatibility**: file an issue against `doitsujin/dxvk`
  upstream OR investigate the root cause and patch our pinned source.
  Per RULE №1, do NOT add a workaround in mtr-asi to mask a DXVK bug.
