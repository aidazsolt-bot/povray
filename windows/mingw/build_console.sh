#!/bin/bash
# MinGW-w64 cross-build of Windows POV-Ray (scrapes windows/vs* source lists).
# Frontend: MINGW_FRONTEND=console (default) or gui.
# console -> $MINGW_OUT/povray.exe
# gui     -> $MINGW_OUT/pvengine64.exe
#
# Autotools Unix-VFE cross remains at windows/mingw/build_autotools_console.sh.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
FRONTEND="${MINGW_FRONTEND:-console}"
OUT="${MINGW_OUT:-$ROOT/build-mingw}"
# Separate object trees so console (_CONSOLE) and gui can share $OUT.
OBJ="$OUT/obj-$FRONTEND"
LOG="$OUT/build-$FRONTEND.log"
CXX="${CXX:-x86_64-w64-mingw32-g++}"
CC="${CC:-x86_64-w64-mingw32-gcc}"
WINDRES="${WINDRES:-x86_64-w64-mingw32-windres}"
JOBS="${JOBS:-$(nproc)}"
# Prefer vs2022 source lists; fall back to vs2015.
VSDIR="$ROOT/windows/vs2022"
[[ -d "$VSDIR" && -f "$VSDIR/povray.vcxproj" ]] || VSDIR="$ROOT/windows/vs2015"

case "$FRONTEND" in
  console|gui) ;;
  *)
    echo "MINGW_FRONTEND must be console or gui (got: $FRONTEND)" >&2
    exit 1
    ;;
esac

for tool in "$CXX" "$CC" "$WINDRES" rg realpath md5sum python3; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "Missing tool: $tool" >&2
    exit 1
  fi
done

mkdir -p "$OBJ"
: > "$LOG"

if [[ "$FRONTEND" == "gui" ]]; then
  PROJECTS=(
    povray
    rtrsupport
    vfewin
    povbase
    povcore
    povbackend
    povfrontend
    povparser
    povms
    povvm
    povplatform
    zlib
    libpng
    jpeg
    tiff
  )
  EXE_NAME="pvengine64.exe"
else
  PROJECTS=(
    console
    vfewin
    povbase
    povcore
    povbackend
    povfrontend
    povparser
    povms
    povvm
    povplatform
    zlib
    libpng
    jpeg
    tiff
  )
  EXE_NAME="povray.exe"
fi

# Collect ClCompile Include= paths from VS projects (relative to windows/vs*).
mapfile -t REL_SRCS < <(
  for p in "${PROJECTS[@]}"; do
    rg -oN 'ClCompile Include="([^"]+)"' -r '$1' "$VSDIR/$p.vcxproj" || true
  done | sed 's|\\|/|g' | while read -r rel; do
    realpath -m "$VSDIR/$rel"
  done | awk '!seen[$0]++'
)

# Skip OpenEXR for now (multi-lib IlmImf/Half/Iex chain not wired yet).
# Skip povms.c: VS excludes it; povms.cpp pulls it in as C++.
FILTERED=()
for s in "${REL_SRCS[@]}"; do
  case "$s" in
    */openexr.cpp) continue ;;
    */povms/povms.c) continue ;;
  esac
  FILTERED+=("$s")
done

# tif_extension.c is not listed in tiff.vcxproj but provides TIFFGetTagList*
# symbols referenced by tif_print.c (needed for a complete static MinGW link).
FILTERED+=("$ROOT/libraries/tiff/libtiff/tif_extension.c")

# libpng expects pnglibconf.h (VS generates it; use upstream prebuilt).
# libtiff expects tif_config.h / tiffconf.h (VS uses the .vc templates).
mkdir -p "$OUT/include"
cp -n "$ROOT/libraries/png/scripts/pnglibconf.h.prebuilt" "$OUT/include/pnglibconf.h"
cp -f "$ROOT/libraries/tiff/libtiff/tif_config.h.vc" "$OUT/include/tif_config.h"
cp -f "$ROOT/libraries/tiff/libtiff/tiffconf.h.vc" "$OUT/include/tiffconf.h"

INCLUDES=(
  -I"$OUT/include"
  -I"$ROOT/windows/povconfig"
  -I"$ROOT/windows"
  -I"$ROOT/windows/rtrsupport"
  -I"$ROOT/source"
  -I"$ROOT/source/base"
  -I"$ROOT/source/backend"
  -I"$ROOT/source/frontend"
  -I"$ROOT/platform/windows"
  -I"$ROOT/platform/x86"
  -I"$ROOT/vfe"
  -I"$ROOT/vfe/win"
  -I"$ROOT/libraries/boost"
  -I"$ROOT/libraries/zlib"
  -I"$ROOT/libraries/png"
  -I"$ROOT/libraries/jpeg"
  -I"$ROOT/libraries/tiff/libtiff"
)

DEFS=(
  -DNDEBUG
  -DWIN32
  -D_WINDOWS
  -DNOMINMAX
  # Note: omit WIN32_LEAN_AND_MEAN so tif_win32.c gets MessageBox/LPTSTR from windows.h
  -DBOOST_ALL_NO_LIB
  -D_CRT_SECURE_NO_DEPRECATE
  -D_WIN32_WINNT=0x0601
  -DWINVER=0x0601
  -DBUILDING_AMD64=1
  -DOPENEXR_MISSING
  # POV-Ray's tiff.cpp uses TIFFClientOpen with void* client handles.
  # USE_WIN32_FILEIO is applied only to libraries/tiff/*.c below.
  -DAVOID_WIN32_FILEIO
  -DBUILT_BY='"MinGW-w64 cross (povray.git Windows VFE)"'
)

if [[ "$FRONTEND" == "console" ]]; then
  DEFS+=(-D_CONSOLE)
else
  DEFS+=(
    -DCOMMONCTRL_VERSION=0x500
    -DCLASSLIB_DEFS_H
    -DISOLATION_AWARE_ENABLED
  )
fi

CXXFLAGS=(-std=c++17 -O2 -ffast-math -pthread "${DEFS[@]}" "${INCLUDES[@]}")
# GUI sources use MSVC-tolerant initializers (e.g. -1 into WORD/unsigned).
if [[ "$FRONTEND" == "gui" ]]; then
  CXXFLAGS+=(-Wno-narrowing)
fi
# tif_dirinfo.c: MSVC-oriented lfind/_lfind callback prototype mismatch under GCC.
CFLAGS=(-O2 -Wno-incompatible-pointer-types "${DEFS[@]}" "${INCLUDES[@]}")

echo "Frontend=$FRONTEND  Sources: ${#FILTERED[@]}  CXX=$CXX  OUT=$OUT/$EXE_NAME" | tee -a "$LOG"

printf '%s\n' "${CFLAGS[@]}" >"$OBJ/cflags.txt"
printf '%s\n' "${CXXFLAGS[@]}" >"$OBJ/cxxflags.txt"

compile_one_file() {
  local src="$1"
  local base hash obj
  base="$(basename "$src")"
  hash="$(printf '%s' "$src" | md5sum | awk '{print $1}')"
  mapfile -t CFLAGS <"$OBJ/cflags.txt"
  mapfile -t CXXFLAGS <"$OBJ/cxxflags.txt"
  case "$src" in
    */povms.c|*/povmsutil.c)
      # POVMS C sources must be compiled as C++ in-tree.
      obj="$OBJ/${base%.c}.$hash.o"
      [[ -f "$obj" && "$obj" -nt "$src" ]] && { echo "SKIP $src"; return 0; }
      echo "CXX $src (as C++)"
      if ! "$CXX" "${CXXFLAGS[@]}" -x c++ -c -o "$obj" "$src" 2>>"$LOG"; then
        echo "FAIL $src" | tee -a "$LOG"
        return 1
      fi
      ;;
    *.c)
      obj="$OBJ/${base%.c}.$hash.o"
      [[ -f "$obj" && "$obj" -nt "$src" ]] && { echo "SKIP $src"; return 0; }
      cextra=()
      case "$src" in
        */libraries/tiff/*)
          # Native Win32 file procs for libtiff itself.
          cextra=(-UAVOID_WIN32_FILEIO -DUSE_WIN32_FILEIO)
          ;;
      esac
      echo "CC  $src ${cextra[*]:-}"
      if ! "$CC" "${CFLAGS[@]}" "${cextra[@]}" -c -o "$obj" "$src" 2>>"$LOG"; then
        echo "FAIL $src" | tee -a "$LOG"
        return 1
      fi
      ;;
    *)
      obj="$OBJ/${base%.cpp}.$hash.o"
      [[ -f "$obj" && "$obj" -nt "$src" ]] && { echo "SKIP $src"; return 0; }
      extra=()
      case "$src" in
        */avx2fma3/*) extra=(-mavx2 -mfma) ;;
        */avxfma4/*)  extra=(-mavx -mfma4) ;;
        */avx/*)      extra=(-mavx) ;;
        */winprecomp.cpp)
          # VS uses this as PCH creator; MinGW just compiles it as a normal TU.
          extra=(-Wno-unused)
          ;;
      esac
      echo "CXX $src ${extra[*]:-}"
      if ! "$CXX" "${CXXFLAGS[@]}" "${extra[@]}" -c -o "$obj" "$src" 2>>"$LOG"; then
        echo "FAIL $src" | tee -a "$LOG"
        return 1
      fi
      ;;
  esac
}
export -f compile_one_file
export CC CXX OBJ LOG OUT

printf '%s\n' "${FILTERED[@]}" >"$OBJ/sources.txt"
if ! cat "$OBJ/sources.txt" | xargs -P "$JOBS" -I{} bash -c 'compile_one_file "$@"' _ {}; then
  echo "Compile failed; see $LOG" >&2
  rg -n 'error:|FATAL|FAIL ' "$LOG" | tail -40 >&2 || true
  exit 1
fi

RC_OBJ=""
if [[ "$FRONTEND" == "gui" ]]; then
  RC_OBJ="$OBJ/pvengine.rc.o"
  RC_MINGW="$ROOT/windows/pvengine_mingw.rc"
  echo "Generate $RC_MINGW (posix path case) + WINDRES" | tee -a "$LOG"
  python3 - "$ROOT/windows" "$RC_MINGW" <<'PY'
import re, sys
from pathlib import Path
win = Path(sys.argv[1])
dst = Path(sys.argv[2])
text = (win / "pvengine.rc").read_text(errors="replace")
index = {}
for p in win.rglob("*"):
    if p.is_file():
        rel = p.relative_to(win).as_posix()
        index[rel.lower()] = rel

def fix_quoted(m):
    s = m.group(1).replace("\\\\", "/").replace("\\", "/")
    return '"' + index.get(s.lower(), s) + '"'

dst.write_text(re.sub(r'"([^"\n]*)"', fix_quoted, text))
PY
  (
    cd "$ROOT/windows"
    "$WINDRES" -I. -Ipovconfig -DNDEBUG -o "$RC_OBJ" "$(basename "$RC_MINGW")"
  ) 2>>"$LOG" || {
    echo "windres failed; see $LOG" >&2
    rg -n 'error:|FATAL|FAIL |undefined|can.t open' "$LOG" | tail -40 >&2 || true
    exit 1
  }
fi

# Collect objects after windres so pvengine.rc.o is included once.
mapfile -t OBJS < <(find "$OBJ" -name '*.o' | sort)

echo "Linking ${#OBJS[@]} objects -> $OUT/$EXE_NAME" | tee -a "$LOG"
# Static libgcc/libstdc++/winpthread for the exe.
LINK_COMMON=(-O2 -pthread -static-libgcc -static-libstdc++
  -Wl,-Bstatic -Wl,--whole-archive -lwinpthread -Wl,--no-whole-archive
  -Wl,-Bdynamic -lws2_32)
LINK_LIBS=()

if [[ "$FRONTEND" == "gui" ]]; then
  # MinGW CRT enters via WinMain; do not use MSVC's POVWINStartup entry.
  LINK_COMMON+=(-mwindows)
  LINK_LIBS+=(
    -lcomctl32 -lgdi32 -luser32 -ladvapi32 -lshell32 -lcomdlg32
    -lole32 -luuid -lwinmm -lversion -lhtmlhelp -lpsapi -lwininet -ldbghelp
  )
fi

"$CXX" "${LINK_COMMON[@]}" -o "$OUT/$EXE_NAME" "${OBJS[@]}" "${LINK_LIBS[@]}" -lstdc++fs 2>>"$LOG" || {
  # retry without libstdc++fs (may be header-only in newer libstdc++)
  "$CXX" "${LINK_COMMON[@]}" -o "$OUT/$EXE_NAME" "${OBJS[@]}" "${LINK_LIBS[@]}" 2>>"$LOG"
}

MINGW_SYSROOT_BIN="/usr/x86_64-w64-mingw32/sys-root/mingw/bin"
copy_runtime_dll() {
  local name="$1"
  shift
  local src
  for src in "$@"; do
    if [[ -f "$src" ]]; then
      cp -f "$src" "$OUT/$name"
      echo "Runtime DLL: $OUT/$name  (from $src)" | tee -a "$LOG"
      return 0
    fi
  done
  echo "WARN: missing runtime DLL $name (searched: $*)" | tee -a "$LOG"
  return 1
}
# Vendored zlib is static-linked; ship winpthread only if anything dynamic needs it.
copy_runtime_dll libwinpthread-1.dll "$MINGW_SYSROOT_BIN/libwinpthread-1.dll" || true

# Optional POVWIN editor DLLs (cmedit + povcmax) from gitignored ./shipped_dll/
if [[ "$FRONTEND" == "gui" ]]; then
  SHIPPED_DLL="${SHIPPED_DLL:-$ROOT/shipped_dll}"
  if [[ -d "$SHIPPED_DLL" ]]; then
    for ed in cmedit64.dll povcmax64.dll cmedit32.dll povcmax32.dll \
              cmedit32-sse2.dll povcmax32-sse2.dll; do
      if [[ -f "$SHIPPED_DLL/$ed" ]]; then
        cp -f "$SHIPPED_DLL/$ed" "$OUT/$ed"
        echo "Editor DLL: $OUT/$ed" | tee -a "$LOG"
      fi
    done
  else
    echo "NOTE: no $SHIPPED_DLL - GUI File/Open editor menus need cmedit64.dll + povcmax64.dll" | tee -a "$LOG"
  fi
fi

file "$OUT/$EXE_NAME" | tee -a "$LOG"
ls -lh "$OUT/$EXE_NAME" "$OUT"/libwinpthread-1.dll \
  "$OUT"/cmedit64.dll "$OUT"/povcmax64.dll 2>/dev/null | tee -a "$LOG" || true
echo "DONE $OUT/$EXE_NAME (frontend=$FRONTEND)"
