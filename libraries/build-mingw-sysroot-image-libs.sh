#!/bin/bash
# Cross-build vendored zlib / libjpeg / libpng / libtiff for MinGW-w64 and
# install static + shared artifacts into the MinGW sysroot.
#
# Uses each library's native build system:
#   zlib  -> win32/Makefile.gcc (MinGW makefile)
#   jpeg  -> GNU Autotools ./configure
#   png   -> scripts/makefile.msys (zip tree; ./configure is a stub)
#   tiff  -> GNU Autotools ./configure
#
# Intended to be run as root (sysroot is root-owned on RPM hosts):
#
#   sudo ./libraries/build-mingw-sysroot-image-libs.sh
#
# Optional env:
#   HOST       default x86_64-w64-mingw32
#   PREFIX     default $($HOST-gcc -print-sysroot)/mingw
#   BUILD_ROOT default /var/tmp/povray-mingw-image-libs
#   JOBS       default nproc
#   PACKAGES   default "zlib jpeg png tiff" (subset ok; deps still enforced)
#
# Does not modify the git working tree: sources are copied into BUILD_ROOT.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HOST="${HOST:-x86_64-w64-mingw32}"
CC="${CC:-${HOST}-gcc}"
CXX="${CXX:-${HOST}-g++}"
AR="${AR:-${HOST}-ar}"
RANLIB="${RANLIB:-${HOST}-ranlib}"
STRIP="${STRIP:-${HOST}-strip}"
WINDRES="${WINDRES:-${HOST}-windres}"
JOBS="${JOBS:-$(nproc)}"
BUILD_ROOT="${BUILD_ROOT:-/var/tmp/povray-mingw-image-libs}"

if [[ -z "${PREFIX:-}" ]]; then
  SYSROOT="$("$CC" -print-sysroot 2>/dev/null || true)"
  if [[ -z "$SYSROOT" || "$SYSROOT" == "/" ]]; then
    SYSROOT="/usr/${HOST}/sys-root"
  fi
  PREFIX="${SYSROOT%/}/mingw"
fi

PACKAGES=(${PACKAGES:-zlib jpeg png tiff})

log() { printf '%s\n' "$*"; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "missing tool: $1"
}

need_cmd "$CC"
need_cmd "$AR"
need_cmd make
need_cmd rsync
need_cmd "$WINDRES"
need_cmd gcc
need_cmd python3

mkdir -p "$PREFIX" 2>/dev/null || true
if [[ ! -d "$PREFIX" ]]; then
  die "PREFIX does not exist and could not be created: $PREFIX"
fi
if [[ ! -w "$PREFIX" ]]; then
  die "PREFIX not writable: $PREFIX (run as root for the MinGW sysroot, or set PREFIX to a writable path)"
fi

export PATH="$(dirname "$(command -v "$CC")"):$PATH"
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
# Do not inherit host CPPFLAGS/CFLAGS/LDFLAGS (e.g. -I.../boost).
export CPPFLAGS="-I$PREFIX/include"
export LDFLAGS="-L$PREFIX/lib"
export CFLAGS="${CFLAGS:--O2}"
export CC CXX AR RANLIB STRIP
unset CXXFLAGS LIBS LDLIBS

log "HOST=$HOST"
log "PREFIX=$PREFIX"
log "BUILD_ROOT=$BUILD_ROOT"
log "PACKAGES=${PACKAGES[*]}"
log "NOTE: this may overwrite distro MinGW packages already in PREFIX (e.g. mingw64-zlib)."

rm -rf "$BUILD_ROOT"
mkdir -p "$BUILD_ROOT"/{src,build}

want() {
  local p
  for p in "${PACKAGES[@]}"; do
    [[ "$p" == "$1" ]] && return 0
  done
  return 1
}

stage_src() {
  local name="$1"
  local src="$ROOT/libraries/$name"
  local dst="$BUILD_ROOT/src/$name"
  [[ -d "$src" ]] || die "missing sources: $src"
  rm -rf "$dst"
  rsync -a --delete \
    --exclude '.git/' \
    --exclude 'autom4te.cache/' \
    "$src/" "$dst/"
}

# --- zlib (native MinGW makefile) -------------------------------------------
build_zlib() {
  log "==> zlib (win32/Makefile.gcc, static+shared)"
  stage_src zlib
  local src="$BUILD_ROOT/src/zlib"
  (
    cd "$src"
    make -f win32/Makefile.gcc clean || true
    make -f win32/Makefile.gcc -j"$JOBS" \
      PREFIX="${HOST}-" \
      SHARED_MODE=1 \
      prefix="$PREFIX" \
      LOC="-O3 -Wall"
    make -f win32/Makefile.gcc install \
      PREFIX="${HOST}-" \
      SHARED_MODE=1 \
      prefix="$PREFIX" \
      INCLUDE_PATH="$PREFIX/include" \
      LIBRARY_PATH="$PREFIX/lib" \
      BINARY_PATH="$PREFIX/bin"
  )
  log "OK zlib -> $PREFIX (libz.a, libz.dll.a, zlib1.dll)"
}

# --- libjpeg (Autotools) ----------------------------------------------------
build_jpeg() {
  log "==> jpeg (configure --host, static+shared)"
  stage_src jpeg
  local src="$BUILD_ROOT/src/jpeg"
  local bld="$BUILD_ROOT/build/jpeg"
  rm -rf "$bld"
  mkdir -p "$bld"
  (
    cd "$bld"
    "$src/configure" \
      --host="$HOST" \
      --prefix="$PREFIX" \
      --enable-shared \
      --enable-static \
      --disable-dependency-tracking \
      CC="$CC" \
      AR="$AR" \
      RANLIB="$RANLIB" \
      STRIP="$STRIP"
    make -j"$JOBS"
    make install
  )
  log "OK jpeg -> $PREFIX"
}

# --- libpng (scripts/makefile.msys; needs zlib) -----------------------------
build_png() {
  log "==> png (scripts/makefile.msys, static+shared)"
  [[ -f "$PREFIX/include/zlib.h" ]] || die "png needs zlib headers in $PREFIX/include (build zlib first)"
  [[ -f "$PREFIX/lib/libz.a" || -f "$PREFIX/lib/libz.dll.a" ]] \
    || die "png needs zlib library in $PREFIX/lib (build zlib first)"
  stage_src png
  local src="$BUILD_ROOT/src/png"
  (
    cd "$src"
    cp -f scripts/makefile.msys Makefile
    # Upstream makefile.msys shared recipe omits OBJS / import-lib. Rewrite
    # those targets in the staged copy only (git tree untouched).
    python3 - "$src/Makefile" <<'PY'
import re, sys
from pathlib import Path
p = Path(sys.argv[1])
text = p.read_text()
repl = """
IMPLIB = libpng.dll.a

shared: $(LIBSO) $(IMPLIB)

$(LIBSO) $(IMPLIB): $(OBJS)
\t$(CC) -shared -o $(LIBSO) -Wl,--out-implib,$(IMPLIB) $(OBJS) -L$(ZLIBLIB) $(LIBS)
\tcp -f $(LIBSO) $(LIBSOMAJ)

"""
text2, n = re.subn(
    r"\nshared:.*?\n\nlibpng\$\(A\):",
    repl + "libpng$(A):",
    text,
    count=1,
    flags=re.S,
)
if n != 1:
    raise SystemExit("failed to patch makefile.msys shared rules")
p.write_text(text2)
PY
    make clean || true
    # Build libs only (skip pngtest.exe - PE cannot run on the Linux host).
    make -j"$JOBS" \
      CC="$CC" \
      LD="$CC" \
      AR_RC="$AR rcs" \
      RANLIB="$RANLIB" \
      prefix="$PREFIX" \
      ZLIBINC="$PREFIX/include" \
      ZLIBLIB="$PREFIX/lib" \
      LIBS="-lz" \
      libpng.a shared
    make install-headers install-static install-shared install-config \
      CC="$CC" \
      LD="$CC" \
      AR_RC="$AR rcs" \
      RANLIB="$RANLIB" \
      prefix="$PREFIX" \
      ZLIBINC="$PREFIX/include" \
      ZLIBLIB="$PREFIX/lib"
    mkdir -p "$PREFIX/bin" "$PREFIX/lib"
    if [[ -f "$src/libpng15.dll" ]]; then
      cp -f "$src/libpng15.dll" "$PREFIX/bin/libpng15.dll"
      cp -f "$src/libpng15.dll" "$PREFIX/bin/libpng.dll"
    elif [[ -f "$PREFIX/lib/libpng15.dll" ]]; then
      cp -f "$PREFIX/lib/libpng15.dll" "$PREFIX/bin/libpng15.dll"
    fi
    if [[ -f "$src/libpng.dll.a" ]]; then
      cp -f "$src/libpng.dll.a" "$PREFIX/lib/libpng.dll.a"
      cp -f "$src/libpng.dll.a" "$PREFIX/lib/libpng15.dll.a"
    fi
  )
  log "OK png -> $PREFIX"
}

# --- libtiff (Autotools; needs zlib + jpeg) ---------------------------------
build_tiff() {
  log "==> tiff (configure --host, static+shared)"
  [[ -f "$PREFIX/include/zlib.h" ]] || die "tiff needs zlib in $PREFIX"
  [[ -f "$PREFIX/include/jpeglib.h" ]] || die "tiff needs jpeg in $PREFIX"
  stage_src tiff
  local src="$BUILD_ROOT/src/tiff"
  local bld="$BUILD_ROOT/build/tiff"
  rm -rf "$bld"
  mkdir -p "$bld"
  (
    cd "$bld"
    # Old libtiff libtool fails MinGW "file magic" on libz.dll.a and then
    # drops shared libs unless deplibs checks are relaxed.
    export lt_cv_deplibs_check_method=pass_all
    "$src/configure" \
      --host="$HOST" \
      --build="$(gcc -dumpmachine)" \
      --prefix="$PREFIX" \
      --enable-shared \
      --enable-static \
      --disable-dependency-tracking \
      --disable-cxx \
      --with-zlib-include-dir="$PREFIX/include" \
      --with-zlib-lib-dir="$PREFIX/lib" \
      --with-jpeg-include-dir="$PREFIX/include" \
      --with-jpeg-lib-dir="$PREFIX/lib" \
      CC="$CC" \
      CXX="$CXX" \
      AR="$AR" \
      RANLIB="$RANLIB" \
      STRIP="$STRIP" \
      CPPFLAGS="$CPPFLAGS" \
      LDFLAGS="$LDFLAGS" \
      CFLAGS="-O2 -Wno-incompatible-pointer-types"
    # mkg3states is a host helper to regenerate tif_fax3sm.c; that file is
    # already shipped. Drop the program from the cross build.
    sed -i 's/^noinst_PROGRAMS = mkg3states.*/noinst_PROGRAMS =/' libtiff/Makefile
    make -j"$JOBS"
    make install
  )
  log "OK tiff -> $PREFIX"
}

# Dependency order regardless of PACKAGES listing.
if want zlib; then build_zlib; fi
if want jpeg; then build_jpeg; fi
if want png; then build_png; fi
if want tiff; then build_tiff; fi

log "DONE. Installed under $PREFIX"
log "Check:"
ls -l "$PREFIX/lib"/libz.* "$PREFIX/lib"/libpng* "$PREFIX/lib"/libjpeg* "$PREFIX/lib"/libtiff* 2>/dev/null || true
ls -l "$PREFIX/bin"/zlib1.dll "$PREFIX/bin"/libpng*.dll "$PREFIX/bin"/libjpeg*.dll "$PREFIX/bin"/libtiff*.dll 2>/dev/null || true
