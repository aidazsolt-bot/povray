# MinGW-w64 Windows build (console + GUI)

Cross-build of the Windows POV-Ray front-ends by scraping `windows/vs2015`
(or `vs2022`) source lists:

- console (`vfe/win/console`) -> `povray.exe`
- POVWIN GUI (`windows/pv*.cpp`) -> `pvengine64.exe`

This is separate from the experimental Autotools Unix-VFE path
(`build_autotools_console.sh` / `./configure --host=...`). Prefer Visual
Studio for official Windows IDE builds; see `windows/README.md`.

## Requirements

- `x86_64-w64-mingw32-g++` / `gcc` / `windres`
- Bash, `rg`, `realpath`, `md5sum`, `python3`
- Optional: Wine to run the resulting `.exe` on Linux

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

Override output directory or toolchain:

```bash
MINGW_OUT=$HOME/tmp/mingw_pov CXX=x86_64-w64-mingw32-g++ \
  MINGW_FRONTEND=gui ./windows/mingw/build_console.sh
```

| Variable | Default |
|---|---|
| `MINGW_FRONTEND` | `console` (`gui` for POVWIN) |
| `MINGW_OUT` | `<repo>/build-mingw` |
| `CC` / `CXX` / `WINDRES` | `x86_64-w64-mingw32-{gcc,g++,windres}` |
| `JOBS` | `nproc` |
| `SHIPPED_DLL` | `<repo>/shipped_dll` (optional editor DLLs) |

PNG/JPEG/Zlib/TIFF come from vendored `libraries/*` (static). OpenEXR is
off (`OPENEXR_MISSING`). The exe links with `-static-libgcc -static-libstdc++`
and static `libwinpthread`.

Optional editor DLLs (`cmedit64.dll`, `povcmax64.dll`) are copied from
`shipped_dll/` when present; POVWIN runs without them.

## Config headers

- `windows/povconfig/syspovconfig.h` - MinGW branch enabled
- `windows/povconfig/syspovconfig_mingw32.h` - MinGW-w64 CRT/config

## Autotools Unix-VFE (alternative)

```bash
./windows/mingw/build_autotools_console.sh
```

That path builds the Unix frontend under `--host=x86_64-w64-mingw32` and
does **not** produce POVWIN.
