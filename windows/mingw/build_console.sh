#!/bin/bash
# MinGW-w64 cross-build of the Windows console POV-Ray front-end.
# Run from repo root or any cwd. Output: windows/mingw/out/povray.exe
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${MINGW_OUT:-$ROOT/windows/mingw/out}"
OBJ="$OUT/obj"
LOG="$OUT/build.log"
CXX="${CXX:-x86_64-w64-mingw32-g++}"
CC="${CC:-x86_64-w64-mingw32-gcc}"
JOBS="${JOBS:-$(nproc)}"

mkdir -p "$OBJ"
: > "$LOG"

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

# Collect ClCompile Include= paths from VS projects (relative to windows/vs2015).
mapfile -t REL_SRCS < <(
  for p in "${PROJECTS[@]}"; do
    rg -oN 'ClCompile Include="([^"]+)"' -r '$1' "$ROOT/windows/vs2015/$p.vcxproj" || true
  done | sed 's|\\|/|g' | while read -r rel; do
    # Resolve relative to windows/vs2015
    realpath -m "$ROOT/windows/vs2015/$rel"
  done | awk '!seen[$0]++'
)

# Skip OpenEXR for now (multi-lib IlmImf/Half/Iex chain not wired yet).
# Skip povms.c: VS excludes it; povms.cpp pulls it in as C++.
# AVX noise TUs are kept; per-file -mavx/-mavx2/-mfma flags are applied below.
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
  -D_CONSOLE
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
  -DBUILT_BY='"MinGW-w64 cross (povray.git)"'
)

CXXFLAGS=(-std=c++17 -O2 -ffast-math -pthread "${DEFS[@]}" "${INCLUDES[@]}")
# tif_dirinfo.c: MSVC-oriented lfind/_lfind callback prototype mismatch under GCC.
CFLAGS=(-O2 -Wno-incompatible-pointer-types "${DEFS[@]}" "${INCLUDES[@]}")

echo "Sources: ${#FILTERED[@]}  CXX=$CXX  OUT=$OUT" | tee -a "$LOG"

compile_one() {
  local src="$1"
  local base hash obj
  base="$(basename "$src")"
  hash="$(printf '%s' "$src" | md5sum | awk '{print $1}')"
  case "$src" in
    *.c)
      obj="$OBJ/${base%.c}.$hash.o"
      if [[ -f "$obj" && "$obj" -nt "$src" ]]; then return 0; fi
      echo "CC  $src" >>"$LOG"
      "$CC" "${CFLAGS[@]}" -c -o "$obj" "$src" >>"$LOG" 2>&1
      ;;
    *)
      obj="$OBJ/${base%.cpp}.$hash.o"
      if [[ -f "$obj" && "$obj" -nt "$src" ]]; then return 0; fi
      echo "CXX $src" >>"$LOG"
      "$CXX" "${CXXFLAGS[@]}" -c -o "$obj" "$src" >>"$LOG" 2>&1
      ;;
  esac
}
export -f compile_one
export CC CXX OBJ LOG
export CFLAGS CXXFLAGS
# bash export arrays not portable; rewrite compile via env files
printf '%s\n' "${CFLAGS[@]}" >"$OUT/cflags.txt"
printf '%s\n' "${CXXFLAGS[@]}" >"$OUT/cxxflags.txt"

compile_one_file() {
  local src="$1"
  local base hash obj
  base="$(basename "$src")"
  hash="$(printf '%s' "$src" | md5sum | awk '{print $1}')"
  mapfile -t CFLAGS <"$OUT/cflags.txt"
  mapfile -t CXXFLAGS <"$OUT/cxxflags.txt"
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

printf '%s\n' "${FILTERED[@]}" >"$OUT/sources.txt"
if ! cat "$OUT/sources.txt" | xargs -P "$JOBS" -I{} bash -c 'compile_one_file "$@"' _ {}; then
  echo "Compile failed; see $LOG" >&2
  rg -n 'error:|FATAL|FAIL ' "$LOG" | tail -40 >&2 || true
  exit 1
fi

mapfile -t OBJS < <(find "$OBJ" -name '*.o' | sort)
echo "Linking ${#OBJS[@]} objects -> $OUT/povray.exe" | tee -a "$LOG"
# Static libgcc/libstdc++/winpthread so the .exe runs on plain Windows without
# shipping MinGW runtime DLLs (libgcc_s_seh-1.dll, libstdc++-6.dll, libwinpthread-1.dll).
# --whole-archive is required; plain -Bstatic -lwinpthread still leaves a DLL import.
LINK_COMMON=(-O2 -pthread -static-libgcc -static-libstdc++
  -Wl,-Bstatic -Wl,--whole-archive -lwinpthread -Wl,--no-whole-archive
  -Wl,-Bdynamic -lws2_32)
"$CXX" "${LINK_COMMON[@]}" -o "$OUT/povray.exe" "${OBJS[@]}" -lstdc++fs 2>>"$LOG" || {
  # retry without libstdc++fs (may be header-only in newer libstdc++)
  "$CXX" "${LINK_COMMON[@]}" -o "$OUT/povray.exe" "${OBJS[@]}" 2>>"$LOG"
}

file "$OUT/povray.exe" | tee -a "$LOG"
ls -lh "$OUT/povray.exe" | tee -a "$LOG"
echo "DONE $OUT/povray.exe"
