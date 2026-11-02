# MinGW-w64 via GNU Autotools

Cross-build the Unix frontend for Windows with the real repo `./configure`
and `make`. Platform selection is Autotools **`--host`** (not `--target`).

This does **not** build POVWIN (`pvengine`). Prefer Visual Studio for the
Windows IDE. See `unix/install.txt` §3.1.4 (cross-compile is BETA).

## Build

```bash
./windows/mingw/build_console.sh
```

Or manually:

```bash
cd unix && ./prebuild.sh
cd ..
mkdir -p build-mingw && cd build-mingw
../configure --host=x86_64-w64-mingw32 \
  COMPILED_BY="your name <email>" \
  --prefix="$PWD/install"
make -j"$(nproc)"
```

Extra configure flags:

```bash
./windows/mingw/build_console.sh -- --disable-shared
```

| Variable | Default |
|---|---|
| `MINGW_HOST` | `x86_64-w64-mingw32` |
| `MINGW_OUT` | `<repo>/build-mingw` |
| `COMPILED_BY` | `MinGW-w64 cross <povray.git>` |
| `CC` / `CXX` | `${MINGW_HOST}-gcc` / `g++` |
| `JOBS` | `nproc` |

Needs MinGW-w64 toolchain plus target libs in the sysroot (`mingw64-zlib`,
and preferably PNG/JPEG/TIFF). Missing optional image libs fall back to
`NON_REDISTRIBUTABLE_BUILD=yes`.

## Branch fixes (minimal)

- `AX_CHECK_LIB*` accepts found libs when cross-compiling (upstream left the
  result unset on that path).
- `unix/povconfig/syspovconfig.h` recognizes `__MINGW32__` / `__MINGW64__`.
- `unix/configure.ac` applies the MinGW TIFF workaround from `$host`.

`make` may still fail in `vfe/unix/` (POSIX console APIs). That is frontend
porting work, not another build system.
