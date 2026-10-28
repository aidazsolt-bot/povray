#!/usr/bin/env python3
"""Compile gsplat_*.comp with glslc and embed as *_spv.inc headers."""
import os
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
DEFAULT_GLSLC = (
    "/home/AndroidDEV/android-sdks/android-sdk_2026_09_16/"
    "ndk/29.0.14206865/shader-tools/linux-x86_64/glslc"
)

def main() -> int:
    glslc = os.environ.get("GLSLC", DEFAULT_GLSLC)
    if not pathlib.Path(glslc).is_file():
        print(f"glslc not found: {glslc}", file=sys.stderr)
        return 1
    out_spv = pathlib.Path(os.path.expanduser("~/tmp/pov-vk"))
    out_spv.mkdir(parents=True, exist_ok=True)
    names = ["gsplat_project", "gsplat_tile_count", "gsplat_tile_write", "gsplat_blend"]
    for name in names:
        src = HERE / f"{name}.comp"
        spv = out_spv / f"{name}.spv"
        cmd = [glslc, "-fshader-stage=compute", str(src), "-o", str(spv)]
        print(" ".join(cmd))
        subprocess.check_call(cmd)
        data = spv.read_bytes()
        if len(data) % 4:
            raise SystemExit(f"{spv}: size not multiple of 4")
        words = [int.from_bytes(data[i : i + 4], "little") for i in range(0, len(data), 4)]
        hdr = HERE / f"{name}_spv.inc"
        with hdr.open("w") as f:
            f.write(f"// Auto-generated from {name}.comp - do not edit.\n")
            f.write(f"static const uint32_t {name}_spv[] = {{\n")
            for i, w in enumerate(words):
                if i % 8 == 0:
                    f.write("  ")
                f.write(f"0x{w:08x}u,")
                f.write("\n" if (i % 8 == 7 or i == len(words) - 1) else " ")
            f.write("};\n")
            f.write(f"static const size_t {name}_spv_size = sizeof({name}_spv);\n")
        print(f"  -> {hdr} ({len(data)} bytes)")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
