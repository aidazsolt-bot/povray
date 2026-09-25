#!/usr/bin/env python3
"""Convert NeRF/3DGS transforms.json (or a single c2w) to a POV-Ray camera .inc.

Emits location + direction/right/up (not look_at), so Kerbl Jacobian fx/fy and
view-dependent SH match the training pose — including roll.

Usage:
  python3 transforms_to_pov_camera.py transforms.json -o cameras/frame0.inc
  python3 transforms_to_pov_camera.py transforms.json --frame 3 --samples 2 \\
      --ply models/Rose.ply -o rose_training_camera.pov --scene

Conventions:
  - transform_matrix is camera-to-world (OpenGL): columns = +X right, +Y up, -Z look.
  - Optional --flip-yz applies the common COLMAP↔NeRF axis fix before export.
"""
from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path
from typing import Any, List, Sequence, Tuple

Vec3 = Tuple[float, float, float]
Mat4 = List[List[float]]


def _vlen(v: Sequence[float]) -> float:
    return math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])


def _vnorm(v: Sequence[float]) -> Vec3:
    n = _vlen(v)
    if n < 1e-12:
        raise ValueError("zero-length vector")
    return (v[0] / n, v[1] / n, v[2] / n)


def _vscale(v: Sequence[float], s: float) -> Vec3:
    return (v[0] * s, v[1] * s, v[2] * s)


def _matmul4(a: Mat4, b: Mat4) -> Mat4:
    out = [[0.0] * 4 for _ in range(4)]
    for i in range(4):
        for j in range(4):
            out[i][j] = sum(a[i][k] * b[k][j] for k in range(4))
    return out


# COLMAP (Y-down) → NeRF/OpenGL (Y-up) style flip sometimes applied to c2w.
_FLIP_YZ: Mat4 = [
    [1.0, 0.0, 0.0, 0.0],
    [0.0, -1.0, 0.0, 0.0],
    [0.0, 0.0, -1.0, 0.0],
    [0.0, 0.0, 0.0, 1.0],
]


def apply_flip_yz(c2w: Mat4) -> Mat4:
    return _matmul4(c2w, _FLIP_YZ)


def fov_from_transforms(data: dict, aspect: float) -> Tuple[float, float, float]:
    """Return (angle_x_deg, right_len, up_len) with |direction|=1."""
    if "camera_angle_x" in data:
        angle_x = math.degrees(float(data["camera_angle_x"]))
    elif data.get("fl_x") and data.get("w"):
        fl = float(data["fl_x"])
        w = float(data["w"])
        angle_x = math.degrees(2.0 * math.atan(0.5 * w / fl))
    else:
        angle_x = 40.0

    # Horizontal full FOV → |Right| / |Direction| = 2 tan(hfov/2)
    right_len = 2.0 * math.tan(math.radians(angle_x) * 0.5)

    if data.get("fl_y") and data.get("h"):
        fl_y = float(data["fl_y"])
        h = float(data["h"])
        angle_y = 2.0 * math.atan(0.5 * h / fl_y)
        up_len = 2.0 * math.tan(angle_y * 0.5)
    elif data.get("w") and data.get("h"):
        up_len = right_len * (float(data["h"]) / float(data["w"]))
    else:
        up_len = right_len / max(aspect, 1e-6)

    return angle_x, right_len, up_len


def c2w_to_pov(c2w: Mat4, right_len: float, up_len: float,
               position_scale: float = 1.0) -> dict:
    """Map OpenGL/nerfstudio c2w to POV-Ray camera fields.

    Prefer look_at+sky+angle (POV builds a clean right/up triad → Kerbl fx/fy
    stay consistent). Also return orthonormal direction/right/up for debugging.

    position_scale: multiply c2w translation (and derived look_at). Use when the
    splat PLY lives in a nerfstudio-normalized frame while transforms.json is
    still in the raw capture frame (DX.GL Apple splatfacto ≈ 0.2).
    """
    loc = (c2w[0][3] * position_scale,
           c2w[1][3] * position_scale,
           c2w[2][3] * position_scale)
    right_gl = _vnorm((c2w[0][0], c2w[1][0], c2w[2][0]))
    up_gl = _vnorm((c2w[0][1], c2w[1][1], c2w[2][1]))
    dir_u = _vnorm((-c2w[0][2], -c2w[1][2], -c2w[2][2]))

    def cross(a: Sequence[float], b: Sequence[float]) -> Vec3:
        return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])

    # POV: right×up=direction. Flip OpenGL right, then Gram-Schmidt.
    right_u = (-right_gl[0], -right_gl[1], -right_gl[2])
    dr = right_u[0]*dir_u[0] + right_u[1]*dir_u[1] + right_u[2]*dir_u[2]
    right_u = _vnorm((right_u[0]-dr*dir_u[0], right_u[1]-dr*dir_u[1], right_u[2]-dr*dir_u[2]))
    up_u = _vnorm(cross(dir_u, right_u))  # ⇒ right×up = dir
    # Aim at the ray's closest approach to world origin (typical object-centric captures).
    t_aim = -(loc[0]*dir_u[0] + loc[1]*dir_u[1] + loc[2]*dir_u[2])
    if t_aim < 0.1 * max(position_scale, 1e-6):
        t_aim = 1.0 * position_scale
    look = (loc[0] + dir_u[0]*t_aim, loc[1] + dir_u[1]*t_aim, loc[2] + dir_u[2]*t_aim)
    return {
        "location": loc,
        "direction": dir_u,
        "right": _vscale(right_u, right_len),
        "up": _vscale(up_u, up_len),
        "sky": up_gl,
        "look_at": look,
    }


def fmt_vec(v: Sequence[float]) -> str:
    return f"<{v[0]:.8f}, {v[1]:.8f}, {v[2]:.8f}>"


def emit_camera_inc(cam: dict, angle_x: float, src: str, frame_idx: int, file_path: str | None) -> str:
    lines = [
        f"// Auto-generated training camera from {src} frame {frame_idx}",
        f"// Do not edit by hand — regenerate with transforms_to_pov_camera.py",
    ]
    if file_path:
        lines.append(f"// Reference image (if present): {file_path}")
    # look_at + sky + angle: POV builds a perpendicular right/up triad (Kerbl needs that).
    lines += [
        "camera {",
        f"  location {fmt_vec(cam['location'])}",
        f"  sky {fmt_vec(cam['sky'])}",
        f"  look_at {fmt_vec(cam['look_at'])}",
        f"  angle {angle_x:.6f}",
        "}",
        "",
    ]
    return "\n".join(lines)


def emit_scene(cam_inc: str, ply: str, samples: int, sh_degree: int) -> str:
    return f"""// Training-view Gaussian splat scene (emission-only; SH is view-baked).
#version 3.8;
global_settings {{ assumed_gamma 1.0 }}

{cam_inc}
background {{ color rgb 1.0 }}

gaussian_splat {{
  "{ply}"
  sh_degree {sh_degree}
  samples {samples}
  alpha_stop 0.999
  opacity_cutoff 0.0039
}}
"""


def load_frame(data: dict, frame_idx: int) -> Tuple[Mat4, str | None]:
    frames = data.get("frames") or []
    if not frames:
        raise SystemExit("transforms.json has no frames[]")
    if frame_idx < 0 or frame_idx >= len(frames):
        raise SystemExit(f"frame {frame_idx} out of range (0..{len(frames)-1})")
    fr = frames[frame_idx]
    c2w = fr.get("transform_matrix")
    if not c2w or len(c2w) < 4:
        raise SystemExit("frame missing transform_matrix")
    return c2w, fr.get("file_path")


def build_aabb_c2w_demo() -> dict:
    """Synthetic transforms.json matching our current Rose AABB framing camera.

    Not a real training view — validates the converter + Kerbl wiring until a
    real capture transforms.json is dropped in.
    """
    # From rose_photo.pov / Assimp AABB framing
    loc = (2.0851, 0.1118, -10.5769)
    look = (-1.7874, -2.6543, 0.4874)
    fwd = _vnorm((look[0] - loc[0], look[1] - loc[1], look[2] - loc[2]))
    # World up preference
    world_up = (0.0, 1.0, 0.0)
    # right = normalize(fwd × up) … wait, right = normalize(fwd × world_up)? 
    # OpenGL: right = normalize(cross(fwd, world_up))? Actually right = normalize(cross(fwd, up)) no:
    # right = normalize(cross(fwd, world_up)) is wrong: cross(fwd, up) gives...
    # Standard: right = normalize(cross(fwd, world_up)) if looking along fwd with Y up:
    #   cross(fwd, world_up) = ?  Actually camera right = normalize(cross(fwd, world_up)) 
    #   when fwd is forward... In right-handed Y-up: right = normalize(cross(fwd, world_up))
    #   No: right = normalize(cross(world_up, fwd))? 
    #   look-at RH Y-up: zaxis = normalize(eye-target) = -fwd if fwd=target-eye
    #   xaxis = normalize(cross(up, zaxis)); yaxis = cross(zaxis, xaxis)
    # With fwd = target-eye: z_cam = -fwd; right = normalize(cross(world_up, z_cam)) = normalize(cross(world_up, -fwd))
    z_cam = _vscale(fwd, -1.0)
    # right = normalize(cross(world_up, z_cam))
    rx = world_up[1] * z_cam[2] - world_up[2] * z_cam[1]
    ry = world_up[2] * z_cam[0] - world_up[0] * z_cam[2]
    rz = world_up[0] * z_cam[1] - world_up[1] * z_cam[0]
    right = _vnorm((rx, ry, rz))
    # up = cross(z_cam, right)
    ux = z_cam[1] * right[2] - z_cam[2] * right[1]
    uy = z_cam[2] * right[0] - z_cam[0] * right[2]
    uz = z_cam[0] * right[1] - z_cam[1] * right[0]
    up = _vnorm((ux, uy, uz))
    # c2w columns: right, up, -fwd (=z_cam), translation
    c2w = [
        [right[0], up[0], z_cam[0], loc[0]],
        [right[1], up[1], z_cam[1], loc[1]],
        [right[2], up[2], z_cam[2], loc[2]],
        [0.0, 0.0, 0.0, 1.0],
    ]
    angle_x = math.radians(40.0)
    return {
        "camera_angle_x": angle_x,
        "w": 640,
        "h": 640,
        "frames": [
            {
                "file_path": "reference/MISSING_put_training_frame_here.png",
                "transform_matrix": c2w,
            }
        ],
        "_comment": "Synthetic AABB framing for Rose — replace with real training transforms.json",
    }


def main(argv: Sequence[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("transforms", nargs="?", help="Path to transforms.json (omit with --write-rose-demo)")
    ap.add_argument("-o", "--output", required=True, help="Output .inc or .pov path")
    ap.add_argument("--frame", type=int, default=0, help="Frame index (default 0)")
    ap.add_argument("--flip-yz", action="store_true", help="Apply COLMAP↔NeRF YZ flip to c2w")
    ap.add_argument("--aspect", type=float, default=1.0, help="Fallback aspect W/H if h missing")
    ap.add_argument("--scene", action="store_true", help="Emit full .pov scene instead of camera .inc only")
    ap.add_argument("--ply", default="models/Rose.ply", help="PLY path inside scene")
    ap.add_argument("--samples", type=int, default=2, help="gaussian_splat samples (2=Kerbl, >=3=volume)")
    ap.add_argument("--sh-degree", type=int, default=3)
    ap.add_argument(
        "--position-scale",
        type=float,
        default=1.0,
        help="Scale c2w translation to match splat PLY frame (DX.GL Apple splatfacto: 0.2)",
    )
    ap.add_argument(
        "--write-rose-demo",
        action="store_true",
        help="Write synthetic Rose AABB transforms.json (path = TRANSFORM or cameras/rose_aabb_c2w.json), then continue",
    )
    args = ap.parse_args(argv)

    if args.write_rose_demo:
        out_json = Path(args.transforms) if args.transforms else Path("cameras/rose_aabb_c2w.json")
        out_json.parent.mkdir(parents=True, exist_ok=True)
        demo = build_aabb_c2w_demo()
        out_json.write_text(json.dumps(demo, indent=2) + "\n")
        print(f"Wrote synthetic transforms: {out_json}", file=sys.stderr)
        args.transforms = str(out_json)

    if not args.transforms:
        ap.error("transforms.json path required (or use --write-rose-demo)")

    path = Path(args.transforms)
    data: dict[str, Any] = json.loads(path.read_text())
    c2w, file_path = load_frame(data, args.frame)
    if args.flip_yz:
        c2w = apply_flip_yz(c2w)

    angle_x, right_len, up_len = fov_from_transforms(data, args.aspect)
    cam = c2w_to_pov(c2w, right_len, up_len, position_scale=args.position_scale)
    inc = emit_camera_inc(cam, angle_x, path.name, args.frame, file_path)

    out = Path(args.output)
    # Guard: never overwrite the transforms.json we just wrote with an .inc
    if out.resolve() == path.resolve():
        print(f"Refusing to overwrite transforms with camera output; pass -o <camera.inc|.pov>", file=sys.stderr)
        return 1
    out.parent.mkdir(parents=True, exist_ok=True)
    if args.scene:
        out.write_text(emit_scene(inc, args.ply, args.samples, args.sh_degree))
    else:
        out.write_text(inc)
    print(f"Wrote {out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
