# MinGW-w64 Windows build (console + GUI)

Cross-build (or native MinGW-w64) of the Windows POV-Ray front-ends:
console (`vfe/win/console`) and optional POVWIN GUI (`windows/pv*.cpp`).

## Requirements

- `x86_64-w64-mingw32-g++` / `gcc` / `windres` (MinGW-w64 toolchain)
- Bash, `rg`, `realpath`, `md5sum`, `python3`
- Optional: Wine, to run the resulting `.exe` on Linux

On Fedora/RHEL-style hosts:

```bash
dnf install mingw64-gcc-c++
```

## Build

From the repository root:

```bash
# Console (default) -> ./build-mingw/povray.exe
./windows/mingw/build_console.sh

# POVWIN GUI -> ./build-mingw/pvengine64.exe
MINGW_FRONTEND=gui ./windows/mingw/build_console.sh
```

Override output directory or toolchain if needed:

```bash
MINGW_OUT=$HOME/tmp/mingw_pov CXX=x86_64-w64-mingw32-g++ ./windows/mingw/build_console.sh
```

Output (default `MINGW_OUT=./build-mingw`):

- Console: `$MINGW_OUT/povray.exe`
- GUI: `$MINGW_OUT/pvengine64.exe` (objs in `obj-console/` vs `obj-gui/`)
- Logs: `$MINGW_OUT/build-console.log` / `build-gui.log`
- This feature branch also copies Assimp runtime DLLs next to the exe:
  `libassimp-6.dll`, `libwebpdecoder.dll` (SOG/WebP), `zlib1.dll`,
  `libwinpthread-1.dll`

The exe itself is linked with `-static-libgcc -static-libstdc++` and static
`libwinpthread`. Ship the Assimp/WebP DLLs beside it for Gaussian-splat imports.

`BUILT_BY` is injected via `-D` (no need to edit `source/base/build.h`).

## Features in this build

| Feature | Status |
|---|---|
| Console VFE front-end (`_CONSOLE`) | Yes |
| PNG / JPEG / Zlib | Yes (vendored `libraries/*`) |
| TIFF | Yes (`tif_config.h.vc` / `tiffconf.h.vc` + `tif_win32.c`; also links `tif_extension.c`) |
| AVX / AVX2 / FMA optimized noise | Yes (runtime CPUID dispatch; TUs built with `-mavx` / `-mavx2 -mfma` / `-mfma4`) |
| OpenEXR (`.exr`) | **No** (`OPENEXR_MISSING`) – IlmImf/Half/Iex chain not wired yet |
| POVWIN GUI (`MINGW_FRONTEND=gui`) | Yes (`pvengine64.exe`; MinGW CRT/`WinMain`, not MSVC `POVWINStartup`) |
| Assimp / Gaussian-splat / Vulkan (volk) | Yes (this feature branch) |

TIFF note: `USE_WIN32_FILEIO` is set **only** when compiling `libraries/tiff/*.c`.
POV-Ray's `source/base/image/tiff.cpp` uses `TIFFClientOpen` with `void*` client
handles and is built with `AVOID_WIN32_FILEIO` instead.

## Config headers

Compiler detection lives in:

- `windows/povconfig/syspovconfig.h` (MinGW branch, no longer `#error`)
- `windows/povconfig/syspovconfig_mingw32.h` (fresh MinGW-w64 config; replaces pre-3.7 headers removed in 2013)

## Notes

- This is an **experimental** MinGW-w64 path, complementary to the official
  Visual Studio build described in `windows/README.md`.
- Cygwin/`unix/configure --without-cygwin-dll` is a separate (still BETA) path;
  it uses `unix/povconfig`, not these Windows MinGW headers.
