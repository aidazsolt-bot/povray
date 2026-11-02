#!/bin/bash
# MinGW-w64 Autotools build (Unix console VFE or POVWIN GUI).
# Optional editor DLLs are copied into the output dir after make.
#
#   ./windows/mingw/build_console.sh
#   MINGW_GUI=true ./windows/mingw/build_console.sh -- --enable-static
#   ./windows/mingw/build_console.sh -- --enable-mingw-gui --enable-static
#
# Extra args after "--" go to ./configure.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HOST="${MINGW_HOST:-x86_64-w64-mingw32}"
OUT="${MINGW_OUT:-$ROOT/build-mingw}"
JOBS="${JOBS:-$(nproc)}"
COMPILED_BY="${COMPILED_BY:-MinGW-w64 cross <povray.git>}"
CXX="${CXX:-${HOST}-g++}"
CC="${CC:-${HOST}-gcc}"
# MINGW_GUI=true|yes|1 enables --enable-mingw-gui unless already in PASSTHRU.
MINGW_GUI_ENV="${MINGW_GUI:-false}"

PASSTHRU=()
if [[ "${1:-}" == "--" ]]; then
  shift
  PASSTHRU=("$@")
elif [[ $# -gt 0 ]]; then
  PASSTHRU=("$@")
fi

for tool in "$CXX" "$CC" make; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "Missing tool: $tool" >&2
    exit 1
  fi
done

newest_src="$ROOT/unix/configure.ac"
for f in "$ROOT/unix/config"/ax_check_lib*.m4 \
         "$ROOT/vfe/Makefile.am" "$ROOT/platform/Makefile.am" \
         "$ROOT/unix/Makefile.am" "$ROOT/source/Makefile.am"; do
  [[ -f "$f" && "$f" -nt "$newest_src" ]] && newest_src="$f"
done
if [[ ! -x "$ROOT/configure" || "$newest_src" -nt "$ROOT/configure" ]]; then
  echo "Running unix/prebuild.sh (aclocal/autoconf/automake)..."
  (cd "$ROOT/unix" && ./prebuild.sh)
fi

echo "Cleaning in-tree host objects/config that would confuse the cross build..."
find "$ROOT/source" "$ROOT/vfe" "$ROOT/platform" "$ROOT/unix" "$ROOT/windows" \
  \( -name '*.o' -o -name '*.lo' -o -name '*.a' -o -name '.libs' -o -name '.deps' \) \
  -print0 2>/dev/null | xargs -0r rm -rf
rm -f "$ROOT/unix/config.h" "$ROOT/config.h" "$ROOT/unix/config.log" "$ROOT/unix/config.status"

mkdir -p "$OUT"
cd "$OUT"

export CPPFLAGS="${CPPFLAGS:-}${CPPFLAGS:+ }-I$ROOT/libraries/boost"

CONFIGURE_ARGS=(
  --host="$HOST"
  --prefix="$OUT/install"
  "COMPILED_BY=$COMPILED_BY"
  CC="$CC"
  CXX="$CXX"
)

have_mingw_gui_flag=0
for a in "${PASSTHRU[@]+"${PASSTHRU[@]}"}"; do
  case "$a" in
    --enable-mingw-gui|--enable-mingw-gui=*|--disable-mingw-gui) have_mingw_gui_flag=1 ;;
  esac
done
case "${MINGW_GUI_ENV,,}" in
  true|yes|1)
    if (( ! have_mingw_gui_flag )); then
      CONFIGURE_ARGS+=(--enable-mingw-gui)
      have_mingw_gui_flag=1
    fi
    ;;
esac

# Image libs (zlib/png/jpeg/tiff): do not pass --with-*=DIR; the MinGW
# cross compiler should find them in its default sysroot. Optional overrides
# remain available via PASSTHRU. SDL/OpenEXR stay off unless requested.
have_passthru_sdl=0
have_passthru_exr=0
for a in "${PASSTHRU[@]+"${PASSTHRU[@]}"}"; do
  case "$a" in
    --with-libsdl|--with-libsdl=*|--without-libsdl) have_passthru_sdl=1 ;;
    --with-openexr|--with-openexr=*|--without-openexr) have_passthru_exr=1 ;;
  esac
done
(( have_passthru_sdl )) || CONFIGURE_ARGS+=(--without-libsdl)
(( have_passthru_exr )) || CONFIGURE_ARGS+=(--without-openexr)

echo "Configuring: --host=$HOST  out=$OUT"
"$ROOT/configure" "${CONFIGURE_ARGS[@]}" "${PASSTHRU[@]+"${PASSTHRU[@]}"}"

echo "Building with make -j$JOBS"
make -j"$JOBS"

GUI_MODE=0
if (( have_mingw_gui_flag )); then
  for a in "${CONFIGURE_ARGS[@]}" "${PASSTHRU[@]+"${PASSTHRU[@]}"}"; do
    case "$a" in
      --enable-mingw-gui|--enable-mingw-gui=yes) GUI_MODE=1 ;;
      --disable-mingw-gui|--enable-mingw-gui=no) GUI_MODE=0 ;;
    esac
  done
fi

if (( GUI_MODE )); then
  BIN="$(find "$OUT" -maxdepth 4 -type f \( -name 'pvengine.exe' -o -name 'pvengine' \) ! -path '*/.libs/*' | head -1 || true)"
  [[ -z "$BIN" && -f "$OUT/unix/.libs/pvengine.exe" ]] && BIN="$OUT/unix/.libs/pvengine.exe"
  if [[ -n "$BIN" ]]; then
    cp -f "$BIN" "$OUT/pvengine64.exe"
    file "$OUT/pvengine64.exe" || true
    ls -lh "$OUT/pvengine64.exe"
  else
    echo "Build finished but pvengine binary not found under $OUT" >&2
    exit 1
  fi
  # Optional POVWIN editor DLLs only (no compile scrape).
  SHIPPED_DLL="${SHIPPED_DLL:-$ROOT/shipped_dll}"
  if [[ -d "$SHIPPED_DLL" ]]; then
    for ed in cmedit64.dll povcmax64.dll cmedit32.dll povcmax32.dll \
              cmedit32-sse2.dll povcmax32-sse2.dll; do
      if [[ -f "$SHIPPED_DLL/$ed" ]]; then
        cp -f "$SHIPPED_DLL/$ed" "$OUT/$ed"
        echo "Editor DLL: $OUT/$ed"
      fi
    done
  else
    echo "NOTE: no $SHIPPED_DLL - GUI editor menus need cmedit64.dll + povcmax64.dll"
  fi
  echo "DONE $OUT/pvengine64.exe (mingw-gui)"
else
  BIN="$(find "$OUT" -maxdepth 4 -type f \( -name 'povray.exe' -o -name 'povray' \) ! -path '*/.libs/*' | head -1 || true)"
  [[ -z "$BIN" && -f "$OUT/unix/.libs/povray.exe" ]] && BIN="$OUT/unix/.libs/povray.exe"
  if [[ -n "$BIN" ]]; then
    cp -f "$BIN" "$OUT/povray.exe"
    file "$OUT/povray.exe" || true
    ls -lh "$OUT/povray.exe"
    echo "DONE $OUT/povray.exe (host=$HOST)"
  else
    echo "Build finished but povray binary not found under $OUT" >&2
    exit 1
  fi
fi
