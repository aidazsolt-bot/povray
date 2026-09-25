#!/usr/bin/env python3
"""Generate a POV-Ray scene stub from a transforms.json (NeRF/3DGS style).

Usage:
  python3 gen_splat_scene.py transforms.json models/Rose.ply > scene.pov

Does not modify Assimp. Emits emission-only gaussian_splat + camera from frame 0.
"""
from __future__ import annotations

import json
import math
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    transforms_path = Path(sys.argv[1])
    ply_path = sys.argv[2]
    data = json.loads(transforms_path.read_text())
    frames = data.get("frames") or []
    if not frames:
        print("No frames in transforms.json", file=sys.stderr)
        return 1
    frame = frames[0]
    c2w = frame.get("transform_matrix")
    if not c2w or len(c2w) < 4:
        print("Missing transform_matrix", file=sys.stderr)
        return 1
    # Camera center = translation of camera-to-world
    loc = (c2w[0][3], c2w[1][3], c2w[2][3])
    # Look along -Z of camera in world
    fwd = (-c2w[0][2], -c2w[1][2], -c2w[2][2])
    look = (loc[0] + fwd[0], loc[1] + fwd[1], loc[2] + fwd[2])
    fl = data.get("fl_x") or data.get("camera_angle_x")
    angle = 40.0
    if "camera_angle_x" in data:
        angle = math.degrees(float(data["camera_angle_x"]))
    elif fl and data.get("w"):
        angle = math.degrees(2.0 * math.atan(0.5 * float(data["w"]) / float(fl)))

    print(f"""// Auto-generated from {transforms_path.name} frame 0 — emission-only splat scene
#version 3.8;
global_settings {{ assumed_gamma 1.0 }}

camera {{
  location <{loc[0]:.6f}, {loc[1]:.6f}, {loc[2]:.6f}>
  look_at <{look[0]:.6f}, {look[1]:.6f}, {look[2]:.6f}>
  angle {angle:.4f}
}}

background {{ color rgb 0.0 }}

gaussian_splat {{
  "{ply_path}"
  sh_degree 3
}}
""")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
