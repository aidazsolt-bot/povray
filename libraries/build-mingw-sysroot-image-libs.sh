#!/bin/bash
# Cross-build vendored zlib / libjpeg / libpng / libtiff / OpenEXR for MinGW-w64
# and install static + shared artifacts into the MinGW sysroot.
#
# Uses each library's native build system:
#   zlib    -> win32/Makefile.gcc (MinGW makefile)
#   jpeg    -> GNU Autotools ./configure
#   png     -> scripts/makefile.msys (zip tree; ./configure is a stub)
#   tiff    -> GNU Autotools ./configure
#   openexr -> GNU Autotools (vendored IlmBase 2.2 + OpenEXR 2.2 subsets;
#              staged copies are patched - tests/tools stripped for cross)
#
# Intended to be run as root (sysroot is root-owned on RPM hosts):
#
#   sudo ./libraries/build-mingw-sysroot-image-libs.sh
#   sudo PACKAGES="openexr" ./libraries/build-mingw-sysroot-image-libs.sh
#
# Optional env:
#   HOST       default x86_64-w64-mingw32
#   PREFIX     default $($HOST-gcc -print-sysroot)/mingw
#   BUILD_ROOT default /var/tmp/povray-mingw-image-libs
#   JOBS       default nproc
#   PACKAGES   default "zlib jpeg png tiff openexr" (subset ok; deps enforced)
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

PACKAGES=(${PACKAGES:-zlib jpeg png tiff openexr})

log() { printf '%s\n' "$*"; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "missing tool: $1"
}

need_cmd "$CC"
need_cmd "$CXX"
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
  # Working tree may have lost +x on configure (ACL cleanup); ensure runnable.
  [[ -f "$dst/configure" ]] && chmod +x "$dst/configure" "$dst/config.guess" "$dst/config.sub" "$dst/install-sh" 2>/dev/null || true
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

# --- IlmBase + OpenEXR 2.2 (vendored subsets; Autotools) --------------------
# POV ships incomplete trees (no *Test / IexMath / IlmImfUtil / tools / doc).
# Stage copies, patch shipped Makefile.in + configure (no autoreconf), skip
# host table generators (eLut.h / b44ExpLogTable.h / dwaLookups.h are present).

# Replace a (possibly multi-line) SUBDIRS = ... assignment in Makefile.am/.in.
_patch_automake_subdirs() {
  local makefile="$1"
  local subdirs="$2"
  [[ -f "$makefile" ]] || return 0
  python3 - "$makefile" "$subdirs" <<'PY'
import pathlib, re, sys
path = pathlib.Path(sys.argv[1])
subdirs = sys.argv[2]
text = path.read_text()
repl = "SUBDIRS = " + subdirs + "\n"
new, n = re.subn(
    r"^SUBDIRS\s*=\s*(?:\\\n|.)*?(?=\n[A-Za-z_]|\n\n|\Z)",
    repl,
    text,
    count=1,
    flags=re.M,
)
if n != 1:
    sys.exit(f"SUBDIRS patch failed in {path}")
path.write_text(new)
PY
}

# Trim ac_config_files in the shipped configure script to paths that exist.
_patch_configure_config_files() {
  local srcdir="$1"
  local configure="$srcdir/configure"
  [[ -f "$configure" ]] || die "missing $configure"
  python3 - "$srcdir" "$configure" <<'PY'
import pathlib, re, sys
srcdir = pathlib.Path(sys.argv[1])
configure = pathlib.Path(sys.argv[2])
text = configure.read_text()
m = re.search(r'^ac_config_files="\$ac_config_files ([^"]*)"', text, re.M)
if not m:
    sys.exit(f"ac_config_files assignment not found in {configure}")
keep = []
for item in m.group(1).split():
    # IlmBase.pc / OpenEXR.pc come from *.pc.in at srcdir root.
    if item.endswith(".pc"):
        cand = srcdir / (item + ".in")
    else:
        cand = srcdir / (item + ".in")
    if cand.is_file():
        keep.append(item)
    else:
        print(f"NOTE: drop missing config file {item} (no {cand.name})", flush=True)
if not keep:
    sys.exit("no ac_config_files entries remain")
repl = 'ac_config_files="$ac_config_files ' + " ".join(keep) + '"'
text2 = text[: m.start()] + repl + text[m.end() :]
# Also drop case arms for removed Makefile outputs (harmless if left, but tidy).
for item in m.group(1).split():
    if item in keep:
        continue
    text2 = re.sub(
        rf'\n    "{re.escape(item)}"\) CONFIG_FILES="\$CONFIG_FILES {re.escape(item)}" ;;',
        "",
        text2,
    )
configure.write_text(text2)
PY
}

_disable_host_table_tools() {
  local am="$1"
  [[ -f "$am" ]] || return 0
  sed -i \
    -e 's/^noinst_PROGRAMS =.*/noinst_PROGRAMS =/' \
    -e 's/^BUILT_SOURCES =.*/BUILT_SOURCES =/' \
    -e 's/^CLEANFILES =.*/CLEANFILES =/' \
    "$am"
  local inn="${am%.am}.in"
  if [[ -f "$inn" ]]; then
    sed -i \
      -e 's/^noinst_PROGRAMS =.*/noinst_PROGRAMS =/' \
      -e 's/^BUILT_SOURCES =.*/BUILT_SOURCES =/' \
      -e 's/^CLEANFILES =.*/CLEANFILES =/' \
      "$inn"
  fi
}

# Headers pick Win32 types under _WIN32; Autotools only lists Posix .cpp.
# Mirror CMake WIN32: compile IlmThread*Win32.cpp and leave HAVE_PTHREAD unset
# (--disable-threading) so the Posix translation units stay empty.
_use_ilmthread_win32() {
  local am="$1/IlmThread/Makefile.am"
  local inn="$1/IlmThread/Makefile.in"
  python3 - "$am" "$inn" <<'PY'
import pathlib, re, sys

def patch(path: pathlib.Path) -> None:
    text = path.read_text()
    # Only treat Win32 as present if it is inside libIlmThread_la_SOURCES
    # (EXTRA_DIST also lists those files).
    src_m = re.search(
        r"^libIlmThread_la_SOURCES =(?:\\\n|.)*?IlmThreadMutexPosix\.cpp",
        text,
        re.M,
    )
    if not src_m:
        sys.exit(f"IlmThread SOURCES block not found in {path}")
    if "IlmThreadWin32.cpp" in src_m.group(0):
        return
    new = text[: src_m.end()] + (
        " IlmThreadWin32.cpp IlmThreadMutexWin32.cpp "
        "IlmThreadSemaphoreWin32.cpp"
    ) + text[src_m.end() :]
    # Makefile.in also hardcodes am_*_OBJECTS; keep them in sync.
    new2, n2 = re.subn(
        r"(^am_libIlmThread_la_OBJECTS =(?:(?:\\\n|.)*?IlmThreadMutexPosix\.lo))",
        r"\1 IlmThreadWin32.lo IlmThreadMutexWin32.lo IlmThreadSemaphoreWin32.lo",
        new,
        count=1,
        flags=re.M,
    )
    if "am_libIlmThread_la_OBJECTS" in text and n2 != 1:
        sys.exit(f"IlmThread Win32 OBJECTS patch failed in {path}")
    path.write_text(new2 if n2 else new)

for path_s in sys.argv[1:]:
    path = pathlib.Path(path_s)
    if path.is_file():
        patch(path)
PY
}

build_ilmbase() {
  log "==> ilmbase (configure --host, static+shared)"
  stage_src ilmbase
  local src="$BUILD_ROOT/src/ilmbase"
  local bld="$BUILD_ROOT/build/ilmbase"
  # Satisfy AC_CONFIG_SRCDIR without shipping the omitted test tree.
  mkdir -p "$src/ImathTest"
  printf 'int main(void){return 0;}\n' >"$src/ImathTest/main.cpp"
  _patch_automake_subdirs "$src/Makefile.am" "Half Iex Imath IlmThread config"
  _patch_automake_subdirs "$src/Makefile.in" "Half Iex Imath IlmThread config"
  _patch_configure_config_files "$src"
  _disable_host_table_tools "$src/Half/Makefile.am"
  _use_ilmthread_win32 "$src"
  rm -rf "$bld"
  mkdir -p "$bld"
  (
    cd "$bld"
    export lt_cv_deplibs_check_method=pass_all
    "$src/configure" \
      --host="$HOST" \
      --build="$(gcc -dumpmachine)" \
      --prefix="$PREFIX" \
      --enable-shared \
      --enable-static \
      --disable-dependency-tracking \
      --disable-threading \
      CC="$CC" \
      CXX="$CXX" \
      AR="$AR" \
      RANLIB="$RANLIB" \
      STRIP="$STRIP" \
      CPPFLAGS="$CPPFLAGS" \
      LDFLAGS="$LDFLAGS" \
      CXXFLAGS="-O2 -std=c++14"
    # Belt-and-suspenders: drop table-tool targets if configure regenerated them.
    if [[ -f Half/Makefile ]]; then
      sed -i \
        -e 's/^noinst_PROGRAMS =.*/noinst_PROGRAMS =/' \
        -e 's/^BUILT_SOURCES =.*/BUILT_SOURCES =/' \
        Half/Makefile
    fi
    make -j"$JOBS"
    make install
  )
  # Vendored tree omits IexMath; drop it from the installed .pc.
  if [[ -f "$PREFIX/lib/pkgconfig/IlmBase.pc" ]]; then
    sed -i 's/ -lIexMath//g' "$PREFIX/lib/pkgconfig/IlmBase.pc"
  fi
  log "OK ilmbase -> $PREFIX (Half/Iex/Imath/IlmThread)"
}

# MinGW is __GNUC__ but has no posix_memalign; use Win32 aligned malloc.
# Also fix pointer-alignment check that casts to unsigned long (LP64 Win64).
_patch_openexr_mingw_headers() {
  local sys="$1/IlmImf/ImfSystemSpecific.h"
  local pix="$1/IlmImf/ImfOptimizedPixelReading.h"
  [[ -f "$sys" ]] || die "missing $sys"
  python3 - "$sys" <<'PY'
import pathlib, sys
path = pathlib.Path(sys.argv[1])
text = path.read_text()
old = """static void* EXRAllocAligned(size_t size, size_t alignment)
{
    void* ptr = 0;
    posix_memalign(&ptr, alignment, size);
    return ptr;
}


static void EXRFreeAligned(void* ptr)
{
    free(ptr);
}"""
new = """static void* EXRAllocAligned(size_t size, size_t alignment)
{
#if defined(_WIN32) || defined(_WIN64)
    return _aligned_malloc(size, alignment);
#else
    void* ptr = 0;
    posix_memalign(&ptr, alignment, size);
    return ptr;
#endif
}


static void EXRFreeAligned(void* ptr)
{
#if defined(_WIN32) || defined(_WIN64)
    _aligned_free(ptr);
#else
    free(ptr);
#endif
}"""
if old not in text:
    if "_aligned_malloc" in text and "posix_memalign" in text:
        pass  # already patched
    else:
        sys.exit(f"EXRAllocAligned block not found in {path}")
else:
    path.write_text(text.replace(old, new, 1))
PY
  if [[ -f "$pix" ]]; then
    sed -i \
      -e 's/((unsigned long)pPointer)/((uintptr_t)pPointer)/' \
      -e 's/unsigned long trailingBits/uintptr_t trailingBits/' \
      "$pix"
    if ! grep -q '#include <stdint.h>' "$pix"; then
      sed -i '1a #include <stdint.h>' "$pix"
    fi
  fi
}

build_openexr() {
  log "==> openexr (configure --host, static+shared; needs ilmbase + zlib)"
  # IlmBase installs headers into include/OpenEXR/
  [[ -d "$PREFIX/include/OpenEXR" ]] || die "missing $PREFIX/include/OpenEXR after ilmbase"
  [[ -f "$PREFIX/include/zlib.h" ]] || die "openexr needs zlib in $PREFIX"
  stage_src openexr
  local src="$BUILD_ROOT/src/openexr"
  local bld="$BUILD_ROOT/build/openexr"
  mkdir -p "$src/IlmImfTest"
  printf 'int main(void){return 0;}\n' >"$src/IlmImfTest/main.cpp"
  _patch_automake_subdirs "$src/Makefile.am" "config IlmImf"
  _patch_automake_subdirs "$src/Makefile.in" "config IlmImf"
  _patch_configure_config_files "$src"
  _disable_host_table_tools "$src/IlmImf/Makefile.am"
  _patch_openexr_mingw_headers "$src"
  rm -rf "$bld"
  mkdir -p "$bld"
  (
    cd "$bld"
    export lt_cv_deplibs_check_method=pass_all
    export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"
    export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
    "$src/configure" \
      --host="$HOST" \
      --build="$(gcc -dumpmachine)" \
      --prefix="$PREFIX" \
      --enable-shared \
      --enable-static \
      --disable-dependency-tracking \
      --disable-imfexamples \
      --disable-imffuzztest \
      --disable-imfhugetest \
      --with-ilmbase-prefix="$PREFIX" \
      CC="$CC" \
      CXX="$CXX" \
      AR="$AR" \
      RANLIB="$RANLIB" \
      STRIP="$STRIP" \
      CPPFLAGS="-I$PREFIX/include -I$PREFIX/include/OpenEXR" \
      LDFLAGS="-L$PREFIX/lib" \
      CXXFLAGS="-O2 -std=c++14" \
      PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig" \
      PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
    if [[ -f IlmImf/Makefile ]]; then
      sed -i \
        -e 's/^noinst_PROGRAMS =.*/noinst_PROGRAMS =/' \
        -e 's/^BUILT_SOURCES =.*/BUILT_SOURCES =/' \
        IlmImf/Makefile
    fi
    make -j"$JOBS"
    make install
  )
  # Ensure runtime DLLs land in bin/ for Windows PATH next to povray.exe.
  mkdir -p "$PREFIX/bin"
  shopt -s nullglob
  for dll in "$PREFIX/bin"/libIlmImf*.dll "$PREFIX/lib"/libIlmImf*.dll \
             "$PREFIX/bin"/libHalf*.dll "$PREFIX/lib"/libHalf*.dll \
             "$PREFIX/bin"/libIex*.dll "$PREFIX/lib"/libIex*.dll \
             "$PREFIX/bin"/libImath*.dll "$PREFIX/lib"/libImath*.dll \
             "$PREFIX/bin"/libIlmThread*.dll "$PREFIX/lib"/libIlmThread*.dll; do
    cp -f "$dll" "$PREFIX/bin/" 2>/dev/null || true
  done
  shopt -u nullglob
  log "OK openexr -> $PREFIX (IlmImf + OpenEXR.pc)"
}

# Dependency order regardless of PACKAGES listing.
if want zlib; then build_zlib; fi
if want jpeg; then build_jpeg; fi
if want png; then build_png; fi
if want tiff; then build_tiff; fi
# openexr implies ilmbase (and needs zlib already in PREFIX).
if want openexr; then
  build_ilmbase
  build_openexr
fi

log "DONE. Installed under $PREFIX"
log "Check:"
ls -l "$PREFIX/lib"/libz.* "$PREFIX/lib"/libpng* "$PREFIX/lib"/libjpeg* "$PREFIX/lib"/libtiff* 2>/dev/null || true
ls -l "$PREFIX/lib"/libIlmImf* "$PREFIX/lib"/libHalf* "$PREFIX/lib"/libImath* 2>/dev/null || true
ls -l "$PREFIX/lib/pkgconfig"/OpenEXR.pc "$PREFIX/lib/pkgconfig"/IlmBase.pc 2>/dev/null || true
ls -l "$PREFIX/bin"/zlib1.dll "$PREFIX/bin"/libpng*.dll "$PREFIX/bin"/libjpeg*.dll "$PREFIX/bin"/libtiff*.dll 2>/dev/null || true
ls -l "$PREFIX/bin"/libIlmImf*.dll "$PREFIX/bin"/libHalf*.dll 2>/dev/null || true
