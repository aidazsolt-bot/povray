# MinGW-w64 Windows console build

Cross-build (or native MinGW-w64) of the **Windows console** POV-Ray front-end
(`vfe/win/console`), without the POVWIN GUI.

## Requirements

- `x86_64-w64-mingw32-g++` / `gcc` (MinGW-w64 toolchain)
- Bash, `rg`, `realpath`, `md5sum`
- Optional: Wine, to run the resulting `.exe` on Linux

On Fedora/RHEL-style hosts:

```bash
dnf install mingw64-gcc-c++
```

## Build

From the repository root:

```bash
./windows/mingw/build_console.sh
```

Override output directory or toolchain if needed:

```bash
MINGW_OUT=$HOME/tmp/mingw_pov/out CXX=x86_64-w64-mingw32-g++ ./windows/mingw/build_console.sh
```

Output:

- `$MINGW_OUT/povray.exe` (default: `windows/mingw/out/povray.exe`)
- Build log: `$MINGW_OUT/build.log`

The link step uses `-static-libgcc -static-libstdc++` and a static
`libwinpthread`, so a normal Windows 64-bit machine does **not** need
MinGW DLLs next to the exe. Copy only `povray.exe`.

`BUILT_BY` is injected via `-D` (no need to edit `source/base/build.h`).

## Features in this build

| Feature | Status |
|---|---|
| Console VFE front-end (`_CONSOLE`) | Yes |
| PNG / JPEG / Zlib | Yes (vendored `libraries/*`) |
| TIFF | Yes (`tif_config.h.vc` / `tiffconf.h.vc` + `tif_win32.c`; also links `tif_extension.c`) |
| AVX / AVX2 / FMA optimized noise | Yes (runtime CPUID dispatch; TUs built with `-mavx` / `-mavx2 -mfma` / `-mfma4`) |
| OpenEXR (`.exr`) | **No** (`OPENEXR_MISSING`) – IlmImf/Half/Iex chain not wired yet |

TIFF note: `USE_WIN32_FILEIO` is set **only** when compiling `libraries/tiff/*.c`.
POV-Ray's `source/base/image/tiff.cpp` uses `TIFFClientOpen` with `void*` client
handles and is built with `AVOID_WIN32_FILEIO` instead.
| POVWIN GUI / custom `POVWINStartup` entry | **No** – console only |
| Assimp / Gaussian-splat experimental parsers | **No** on this branch base (upstream `master`); add when merging those feature branches |

## Config headers

Compiler detection lives in:

- `windows/povconfig/syspovconfig.h` (MinGW branch, no longer `#error`)
- `windows/povconfig/syspovconfig_mingw32.h` (fresh MinGW-w64 config; replaces pre-3.7 headers removed in 2013)

## Notes

- This is an **experimental** console path for MinGW-w64, complementary to the
  official Visual Studio build described in `windows/README.md`.
- Cygwin/`unix/configure --without-cygwin-dll` is a separate (still BETA) path;
  it uses `unix/povconfig`, not these Windows MinGW headers.
