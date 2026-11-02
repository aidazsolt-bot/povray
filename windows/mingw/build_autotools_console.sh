#!/bin/bash
# Thin MinGW cross driver: real GNU configure (--host) + make.
# Extra args after "--" go to ./configure.
#
#   ./windows/mingw/build_console.sh
#   MINGW_OUT=$HOME/tmp/mingw_pov ./windows/mingw/build_console.sh -- --disable-shared
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HOST="${MINGW_HOST:-x86_64-w64-mingw32}"
OUT="${MINGW_OUT:-$ROOT/build-mingw}"
JOBS="${JOBS:-$(nproc)}"
COMPILED_BY="${COMPILED_BY:-MinGW-w64 cross <povray.git>}"
CXX="${CXX:-${HOST}-g++}"
CC="${CC:-${HOST}-gcc}"
SYSROOT="${MINGW_SYSROOT:-$("$CXX" -print-sysroot 2>/dev/null)/mingw}"
SYSROOT="${SYSROOT%/}"

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
for f in "$ROOT/unix/config"/ax_check_lib*.m4; do
  [[ -f "$f" && "$f" -nt "$newest_src" ]] && newest_src="$f"
done
if [[ ! -x "$ROOT/configure" || "$newest_src" -nt "$ROOT/configure" ]]; then
  echo "Running unix/prebuild.sh (aclocal/autoconf/automake)..."
  (cd "$ROOT/unix" && ./prebuild.sh)
fi

# VPATH / quoted #include "config.h" would pick up a stale in-tree Linux
# unix/config.h (and host *.o) ahead of the MinGW out-of-tree build.
echo "Cleaning in-tree host objects/config that would confuse the cross build..."
find "$ROOT/source" "$ROOT/vfe" "$ROOT/platform" "$ROOT/unix" \
  \( -name '*.o' -o -name '*.lo' -o -name '*.a' -o -name '.libs' -o -name '.deps' \) \
  -print0 2>/dev/null | xargs -0r rm -rf
rm -f "$ROOT/unix/config.h" "$ROOT/config.h" "$ROOT/unix/config.log" "$ROOT/unix/config.status"


mkdir -p "$OUT"
cd "$OUT"

# Vendored Boost headers (AX_BOOST_BASE skips system search when cross-compiling).
export CPPFLAGS="${CPPFLAGS:-}${CPPFLAGS:+ }-I$ROOT/libraries/boost"

CONFIGURE_ARGS=(
  --host="$HOST"
  --prefix="$OUT/install"
  "COMPILED_BY=$COMPILED_BY"
  CC="$CC"
  CXX="$CXX"
)

append_with_or_without() {
  local opt="$1" libfile="$2"
  local a
  for a in "${PASSTHRU[@]+"${PASSTHRU[@]}"}"; do
    case "$a" in
      --with-${opt}|--with-${opt}=*|--without-${opt}) return 0 ;;
    esac
  done
  if [[ -n "$SYSROOT" && -e "$SYSROOT/lib/$libfile" ]]; then
    CONFIGURE_ARGS+=("--with-${opt}=$SYSROOT/lib")
  else
    CONFIGURE_ARGS+=("--without-${opt}")
    NEED_NON_REDIST=1
  fi
}

NEED_NON_REDIST=0
append_with_or_without zlib libz.dll.a
append_with_or_without libpng libpng.dll.a
if [[ -e "$SYSROOT/lib/libjpeg.dll.a" || -e "$SYSROOT/lib/libjpeg.a" ]]; then
  append_with_or_without libjpeg libjpeg.dll.a
elif [[ -e "$SYSROOT/lib/libjpeg-62.dll.a" ]]; then
  append_with_or_without libjpeg libjpeg-62.dll.a
else
  append_with_or_without libjpeg libjpeg.dll.a
fi
append_with_or_without libtiff libtiff.dll.a

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

if (( NEED_NON_REDIST )); then
  already=0
  for a in "${PASSTHRU[@]+"${PASSTHRU[@]}"}"; do
    case "$a" in NON_REDISTRIBUTABLE_BUILD=*) already=1 ;; esac
  done
  (( already )) || CONFIGURE_ARGS+=("NON_REDISTRIBUTABLE_BUILD=yes")
  echo "NOTE: some image libs missing in $SYSROOT - using NON_REDISTRIBUTABLE_BUILD" >&2
fi

NEED_CONFIGURE=1
if [[ -f config.status ]] && (( ${#PASSTHRU[@]} == 0 )); then
  if ./config.status --config 2>/dev/null | grep -F -- "--host=$HOST" >/dev/null 2>&1; then
    NEED_CONFIGURE=0
  fi
fi

if (( NEED_CONFIGURE )); then
  echo "Configuring: --host=$HOST  out=$OUT"
  "$ROOT/configure" "${CONFIGURE_ARGS[@]}" "${PASSTHRU[@]+"${PASSTHRU[@]}"}"
fi

echo "Building with make -j$JOBS"
make -j"$JOBS"

BIN="$(find "$OUT" -maxdepth 4 -type f \( -name 'povray.exe' -o -name 'povray' \) ! -path '*/.libs/*' | head -1 || true)"
if [[ -z "$BIN" && -f "$OUT/unix/.libs/povray.exe" ]]; then
  BIN="$OUT/unix/.libs/povray.exe"
fi
if [[ -n "$BIN" ]]; then
  cp -f "$BIN" "$OUT/povray.exe"
  file "$OUT/povray.exe" || true
  ls -lh "$OUT/povray.exe"
  echo "DONE $OUT/povray.exe (host=$HOST)"
else
  echo "Build finished but povray binary not found under $OUT" >&2
  exit 1
fi
