# MinGW-w64 via GNU Autotools

Cross-build with the real repo `./configure` and `make`.

| Frontend | Configure | Output |
|---|---|---|
| Console (Unix VFE) | default | `povray.exe` |
| POVWIN GUI | `--enable-mingw-gui` | `pvengine` / `pvengine64.exe` |

## Build

```bash
# Console
./windows/mingw/build_console.sh -- --enable-static

# POVWIN GUI (MinGW host required)
MINGW_GUI=true ./windows/mingw/build_console.sh -- --enable-static
# same as:
./windows/mingw/build_console.sh -- --enable-mingw-gui --enable-static
```

Or manually:

```bash
cd unix && ./prebuild.sh && cd ..
mkdir -p build-mingw && cd build-mingw
../configure --host=x86_64-w64-mingw32 \
  --enable-mingw-gui \
  --enable-static \
  COMPILED_BY="your name <email>" \
  --prefix="$PWD/install"
make -j"$(nproc)"
```

After a GUI build, optional editor DLLs (`cmedit64.dll`, `povcmax64.dll`) are
copied from `shipped_dll/` into the output directory when present.

| Variable | Default |
|---|---|
| `MINGW_HOST` | `x86_64-w64-mingw32` |
| `MINGW_OUT` | `<repo>/build-mingw` |
| `MINGW_GUI` | `false` (`true` adds `--enable-mingw-gui`) |
| `COMPILED_BY` | `MinGW-w64 cross <povray.git>` |
| `SHIPPED_DLL` | `<repo>/shipped_dll` |

Needs MinGW-w64 plus target image libs in the sysroot (`zlib`, `libpng`,
`jpeg`, `tiff`). See `libraries/build-mingw-sysroot-image-libs.sh`.
