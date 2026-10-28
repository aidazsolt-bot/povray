#!/usr/bin/env bash
# Rebuild POV-Ray with Gaussian splat Vulkan compute enabled.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ASSIMP="${ASSIMP:-/usr/src/TestAdnroidNDKSDK/Assimp-Arris.Git.2026}"
VKH="${VK_HEADERS:-/usr/src/TestAdnroidNDKSDK/RenderAssimp2026/desktop/deps/vulkan-headers}"
GLSLC="${GLSLC:-/home/AndroidDEV/android-sdks/android-sdk_2026_09_16/ndk/29.0.14206865/shader-tools/linux-x86_64/glslc}"

if [[ "${1:-}" == "--shaders" ]]; then
  GLSLC="$GLSLC" python3 "$ROOT/source/core/shape/shaders/embed_spv.py"
fi

COMMON=(
  -DHAVE_CONFIG_H -I. -I../unix -I../unix/povconfig -I../platform/unix -I../platform/x86
  -I../unix -I../vfe -I../vfe/unix -I/usr/include/SDL -D_GNU_SOURCE=1 -D_REENTRANT
  -DPOV_PARSER_EXPERIMENTAL_OBJ_IMPORT=1 -DPOV_PARSER_EXPERIMENTAL_ASSIMP_IMPORT=1
  -I"$ASSIMP/include" -I"$ASSIMP/build-host/include" -I/usr/include
  -DPOV_HAVE_VULKAN -I"$ROOT/libraries/volk" -I"$VKH/include"
  -pipe -Wno-multichar -Wno-write-strings -fno-enforce-eh-specs -Wno-non-template-friend
  -O2 -fno-fast-math -march=native -pthread
)

cd "$ROOT/source"
g++ "${COMMON[@]}" -c -o core/shape/gaussiansplat_vk.o core/shape/gaussiansplat_vk.cpp
g++ "${COMMON[@]}" -c -o core/shape/gaussiansplat.o core/shape/gaussiansplat.cpp
g++ "${COMMON[@]}" -c -o parser/reservedwords.o parser/reservedwords.cpp
g++ "${COMMON[@]}" -c -o parser/parser_assimp.o parser/parser_assimp.cpp
g++ "${COMMON[@]}" -c -o backend/povray.o backend/povray.cpp
ar cru libpovray.a core/shape/gaussiansplat.o core/shape/gaussiansplat_vk.o \
  parser/reservedwords.o parser/parser_assimp.o backend/povray.o
ranlib libpovray.a

cd "$ROOT/unix"
g++ -pipe -O2 -fno-fast-math -pthread -L/usr/lib64 -o povray \
  disp_sdl.o disp_text.o \
  ../vfe/libvfe.a ../source/libpovray.a ../vfe/libvfe.a \
  ../platform/libplatform.a ../platform/libx86.a \
  ../platform/libx86avx.a ../platform/libx86avxfma4.a ../platform/libx86avx2fma3.a \
  -L"$ASSIMP/build-host/bin" -lassimp -Wl,-rpath,"$ASSIMP/build-host/bin" \
  /usr/lib64/libvulkan.so.1 -lSDL -lXpm -lX11 -ltiff -ljpeg -lpng -lz -lrt -lm -pthread

echo "Built $ROOT/unix/povray (Vulkan gsplat enabled)"
echo "Scene token: backend vulkan   (falls back to CPU if device init fails)"
