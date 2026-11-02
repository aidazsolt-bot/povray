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
# gnu++14: toolchain default is C++17, but OpenEXR 2.2 Imath headers still use
# dynamic exception specifications (removed in C++17). POV only requires C++11.
#
# --enable-static / --disable-shared: let configure's AX_PROG_LD_STATIC add
# -static; do not mix in -Wl,-Bdynamic (that defeats a full static link).
# Default (shared image/Assimp DLLs): still ship without libgcc/libstdc++/
# winpthread DLLs via AX_PTHREAD's PTHREAD_LIBS (LIBS append in configure.ac).
want_full_static=0
for a in "${PASSTHRU[@]+"${PASSTHRU[@]}"}"; do
  case "$a" in
    --enable-static|--enable-static=*|--disable-shared|--disable-shared=*) want_full_static=1 ;;
  esac
done
export CXXFLAGS="${CXXFLAGS:-}${CXXFLAGS:+ }-std=gnu++14 -pthread"
export PTHREAD_CFLAGS=""
if (( want_full_static )); then
  export LDFLAGS="${LDFLAGS:-}"
  export PTHREAD_LIBS="-lpthread"
  # Static Assimp needs its private deps on the link line (AX_CHECK_ASSIMP
  # only adds -lassimp). zlib is already pulled by POV's image-lib checks.
  # Put -L before those -l flags so early configure link tests still work.
  assimp_root=""
  want_assimp=0
  for a in "${PASSTHRU[@]+"${PASSTHRU[@]}"}"; do
    case "$a" in
      --enable-assimp|--enable-assimp=*) want_assimp=1 ;;
      --with-assimp=*)
        want_assimp=1
        assimp_root="${a#--with-assimp=}"
        ;;
    esac
  done
  if (( want_assimp )); then
    if [[ -n "$assimp_root" && -d "$assimp_root/lib" ]]; then
      export LDFLAGS="-L$assimp_root/lib ${LDFLAGS}"
    fi
    export LIBS="${LIBS:-}${LIBS:+ }-ldraco -lwebpdecoder"
  fi
else
  export CXXFLAGS="${CXXFLAGS} -static-libgcc -static-libstdc++"
  export LDFLAGS="${LDFLAGS:-}${LDFLAGS:+ }-static-libgcc -static-libstdc++"
  export PTHREAD_LIBS="-Wl,-Bstatic -lstdc++ -lgcc -lgcc_eh -lpthread -lwinpthread -Wl,-Bdynamic"
fi

# Autoconf requires an absolute --prefix. For a portable Windows tree we still
# configure with a dummy absolute prefix, then bake CWD-relative paths into the
# binary via make overrides (prefix=./etc + ./share) and stage into $OUT/portable.
# Set MINGW_PORTABLE=0 to keep the configure prefix as compiled-in paths.
# Optional absolute Windows prefix: MINGW_PREFIX=C:/POV-Ray (no portable mode).
MINGW_PORTABLE="${MINGW_PORTABLE:-1}"
MINGW_PREFIX="${MINGW_PREFIX:-/usr/local}"
have_passthru_prefix=0
for a in "${PASSTHRU[@]+"${PASSTHRU[@]}"}"; do
  case "$a" in
    --prefix|--prefix=*) have_passthru_prefix=1 ;;
  esac
done

CONFIGURE_ARGS=(
  --host="$HOST"
  "COMPILED_BY=$COMPILED_BY"
  CC="$CC"
  CXX="$CXX"
  LDFLAGS="$LDFLAGS"
  CXXFLAGS="$CXXFLAGS"
  PTHREAD_CFLAGS="$PTHREAD_CFLAGS"
  PTHREAD_LIBS="$PTHREAD_LIBS"
)
(( have_passthru_prefix )) || CONFIGURE_ARGS+=(--prefix="$MINGW_PREFIX")

MAKE_PATH_OVERRIDES=()
case "${MINGW_PORTABLE,,}" in
  1|true|yes)
    if (( ! have_passthru_prefix )) && [[ "$MINGW_PREFIX" == /usr/local ]]; then
      # Relative to the process CWD: ship exe + etc/ + share/ in one folder.
      MAKE_PATH_OVERRIDES=(prefix=. datarootdir=./share sysconfdir=./etc)
    fi
    ;;
esac
# Propagate LIBS (e.g. static Assimp deps) into configure's cache.
if [[ -n "${LIBS:-}" ]]; then
  CONFIGURE_ARGS+=("LIBS=$LIBS")
fi

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

# Image libs (zlib/png/jpeg/tiff/OpenEXR): do not pass --with-*=DIR; the MinGW
# cross compiler / pkg-config should find them in the default sysroot. Optional
# overrides remain available via PASSTHRU. SDL stays off unless requested.
#
# Point pkg-config at the MinGW sysroot so OpenEXR (pkg-config based) is
# detected the same way as the other image libs, without --with-openexr=DIR.
_mingw_sysroot="$("$CC" -print-sysroot 2>/dev/null || true)"
if [[ -z "$_mingw_sysroot" || "$_mingw_sysroot" == "/" ]]; then
  _mingw_sysroot="/usr/${HOST}/sys-root"
fi
_mingw_pc="${_mingw_sysroot%/}/mingw/lib/pkgconfig"
if [[ -d "$_mingw_pc" ]]; then
  export PKG_CONFIG_LIBDIR="$_mingw_pc${PKG_CONFIG_LIBDIR:+:$PKG_CONFIG_LIBDIR}"
  # Do not mix in host .pc files when cross-compiling.
  export PKG_CONFIG_PATH="${PKG_CONFIG_PATH:-}"
fi

have_passthru_sdl=0
for a in "${PASSTHRU[@]+"${PASSTHRU[@]}"}"; do
  case "$a" in
    --with-libsdl|--with-libsdl=*|--without-libsdl) have_passthru_sdl=1 ;;
  esac
done
(( have_passthru_sdl )) || CONFIGURE_ARGS+=(--without-libsdl)

echo "Configuring: --host=$HOST  out=$OUT"
"$ROOT/configure" "${CONFIGURE_ARGS[@]}" "${PASSTHRU[@]+"${PASSTHRU[@]}"}"

echo "Building with make -j$JOBS ${MAKE_PATH_OVERRIDES[*]+${MAKE_PATH_OVERRIDES[*]}}"
make -j"$JOBS" ${MAKE_PATH_OVERRIDES[@]+"${MAKE_PATH_OVERRIDES[@]}"}

GUI_MODE=0
if (( have_mingw_gui_flag )); then
  for a in "${CONFIGURE_ARGS[@]}" "${PASSTHRU[@]+"${PASSTHRU[@]}"}"; do
    case "$a" in
      --enable-mingw-gui|--enable-mingw-gui=yes) GUI_MODE=1 ;;
      --disable-mingw-gui|--enable-mingw-gui=no) GUI_MODE=0 ;;
    esac
  done
fi

PORTABLE="$OUT/portable"
rm -rf "$PORTABLE" "$OUT/portable." "$OUT"/portable-test.
mkdir -p "$PORTABLE"
echo "Installing runtime data into $PORTABLE"
# Trailing slash required: with prefix=. , DESTDIR+prefix must not become 'dir.'.
make install DESTDIR="${PORTABLE}/" ${MAKE_PATH_OVERRIDES[@]+"${MAKE_PATH_OVERRIDES[@]}"} >/dev/null
# Hoist binaries from bin/ to portable root for a flat run-from-here tree.
for b in "$PORTABLE/bin/"*; do
  [[ -f "$b" ]] || continue
  cp -f "$b" "$PORTABLE/$(basename "$b")"
done

if (( GUI_MODE )); then
  BIN="$(find "$OUT" -maxdepth 4 -type f \( -name 'pvengine.exe' -o -name 'pvengine' \) ! -path '*/.libs/*' ! -path '*/portable/*' | head -1 || true)"
  [[ -z "$BIN" && -f "$OUT/unix/.libs/pvengine.exe" ]] && BIN="$OUT/unix/.libs/pvengine.exe"
  if [[ -n "$BIN" ]]; then
    cp -f "$BIN" "$OUT/pvengine64.exe"
    cp -f "$BIN" "$PORTABLE/pvengine64.exe"
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
        cp -f "$SHIPPED_DLL/$ed" "$PORTABLE/$ed"
        echo "Editor DLL: $OUT/$ed"
      fi
    done
  else
    echo "NOTE: no $SHIPPED_DLL - GUI editor menus need cmedit64.dll + povcmax64.dll"
  fi
  echo "DONE $OUT/pvengine64.exe (mingw-gui)"
  echo "PORTABLE tree: $PORTABLE (run from that directory)"
else
  BIN="$(find "$OUT" -maxdepth 4 -type f \( -name 'povray.exe' -o -name 'povray' \) ! -path '*/.libs/*' ! -path '*/portable/*' | head -1 || true)"
  [[ -z "$BIN" && -f "$OUT/unix/.libs/povray.exe" ]] && BIN="$OUT/unix/.libs/povray.exe"
  if [[ -n "$BIN" ]]; then
    cp -f "$BIN" "$OUT/povray.exe"
    cp -f "$BIN" "$PORTABLE/povray.exe"
    file "$OUT/povray.exe" || true
    ls -lh "$OUT/povray.exe"
    echo "DONE $OUT/povray.exe (host=$HOST)"
    echo "PORTABLE tree: $PORTABLE (copy that folder to Windows; run from inside it)"
  else
    echo "Build finished but povray binary not found under $OUT" >&2
    exit 1
  fi
fi

# Sanity: baked paths must not point at the Linux build tree.
_check_bin="$OUT/povray.exe"
(( GUI_MODE )) && _check_bin="$OUT/pvengine64.exe"
if [[ -f "$_check_bin" ]] && x86_64-w64-mingw32-strings "$_check_bin" 2>/dev/null | rg -q '/usr/src/TestAdnroidNDKSDK'; then
  echo "WARNING: binary still contains Linux source-tree paths" >&2
fi
if [[ -f "$_check_bin" ]]; then
  echo "Baked conf/lib paths:"
  x86_64-w64-mingw32-strings "$_check_bin" 2>/dev/null | rg -n 'etc/povray|share/povray' | head -10 || true
fi
