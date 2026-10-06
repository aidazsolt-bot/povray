#!/usr/bin/env python3
"""Extract a triangle mesh from a 3DGS PLY via opacity-field marching cubes.

Uses only Gaussian splat storage (means, scales, quaternions, opacity, SH DC).
No training cameras or images required.

Example:
  python3 tools/splat_to_mesh.py models/Rose.ply -o generated/rose_surface.obj \\
      --res 128 --iso 0.15 --opacity-min 0.05
  python3 tools/splat_to_mesh.py public/apple/splats/apple.ply \\
      -o generated/apple_surface.obj --res 160 --iso 0.12
"""
from __future__ import annotations

import argparse
import math
import os
import struct
import sys
from pathlib import Path

import numpy as np

try:
    from skimage.measure import marching_cubes
except ImportError as exc:  # pragma: no cover
    raise SystemExit(
        "scikit-image is required (pip install --user scikit-image). "
        f"Import failed: {exc}"
    ) from exc


SH_C0 = 0.28209479177387814


def sigmoid(x: np.ndarray) -> np.ndarray:
    return 1.0 / (1.0 + np.exp(-x))


def read_gaussians_ply(path: Path) -> dict[str, np.ndarray]:
    """Parse Inria/Nerfstudio binary little-endian Gaussian splat PLY."""
    with path.open("rb") as f:
        props: list[str] = []
        nvert = 0
        fmt = None
        while True:
            line = f.readline()
            if not line:
                raise ValueError(f"Truncated PLY header: {path}")
            s = line.decode("ascii", "replace").strip()
            if s.startswith("format "):
                fmt = s.split()[1]
            elif s.startswith("element vertex "):
                nvert = int(s.split()[-1])
            elif s.startswith("property "):
                parts = s.split()
                props.append(parts[-1])
            elif s == "end_header":
                break

        if fmt != "binary_little_endian":
            raise ValueError(f"Only binary_little_endian PLY supported, got {fmt!r}")
        if nvert <= 0:
            raise ValueError("No vertices in PLY")

        # All common 3DGS props are float32.
        missing = [p for p in ("x", "y", "z", "opacity", "scale_0", "scale_1", "scale_2",
                               "rot_0", "rot_1", "rot_2", "rot_3") if p not in props]
        if missing:
            raise ValueError(f"PLY missing required properties: {missing}")

        row = struct.Struct("<" + "f" * len(props))
        raw = f.read(row.size * nvert)
        if len(raw) < row.size * nvert:
            raise ValueError("Truncated PLY body")
        flat = np.frombuffer(raw, dtype="<f4").reshape(nvert, len(props))
        col = {name: flat[:, i].astype(np.float64) for i, name in enumerate(props)}

    pos = np.stack([col["x"], col["y"], col["z"]], axis=1)
    opacity = sigmoid(col["opacity"])
    scale = np.exp(np.stack([col["scale_0"], col["scale_1"], col["scale_2"]], axis=1))
    # Nerfstudio / Inria: rot_0..3 = w,x,y,z
    quat = np.stack([col["rot_0"], col["rot_1"], col["rot_2"], col["rot_3"]], axis=1)
    qn = np.linalg.norm(quat, axis=1, keepdims=True)
    qn = np.maximum(qn, 1e-12)
    quat = quat / qn

    if all(k in col for k in ("f_dc_0", "f_dc_1", "f_dc_2")):
        rgb = np.clip(0.5 + SH_C0 * np.stack(
            [col["f_dc_0"], col["f_dc_1"], col["f_dc_2"]], axis=1), 0.0, 1.0)
    else:
        rgb = np.full((nvert, 3), 0.7, dtype=np.float64)

    return {
        "pos": pos,
        "opacity": opacity,
        "scale": scale,
        "quat": quat,
        "rgb": rgb,
    }


def quat_to_rotmats(quat: np.ndarray) -> np.ndarray:
    """(N,4) wxyz -> (N,3,3) rotation matrices."""
    w, x, y, z = quat[:, 0], quat[:, 1], quat[:, 2], quat[:, 3]
    r = np.empty((quat.shape[0], 3, 3), dtype=np.float64)
    r[:, 0, 0] = 1 - 2 * (y * y + z * z)
    r[:, 0, 1] = 2 * (x * y - z * w)
    r[:, 0, 2] = 2 * (x * z + y * w)
    r[:, 1, 0] = 2 * (x * y + z * w)
    r[:, 1, 1] = 1 - 2 * (x * x + z * z)
    r[:, 1, 2] = 2 * (y * z - x * w)
    r[:, 2, 0] = 2 * (x * z - y * w)
    r[:, 2, 1] = 2 * (y * z + x * w)
    r[:, 2, 2] = 1 - 2 * (x * x + y * y)
    return r


def filter_splats(
    g: dict[str, np.ndarray],
    opacity_min: float,
    max_scale: float | None,
) -> dict[str, np.ndarray]:
    op = g["opacity"]
    sc = g["scale"]
    mask = op >= opacity_min
    if max_scale is not None and max_scale > 0:
        mask &= sc.max(axis=1) <= max_scale
    idx = np.nonzero(mask)[0]
    if idx.size == 0:
        raise SystemExit("No splats left after opacity/scale filter")
    return {k: v[idx] for k, v in g.items()}


def build_opacity_grid(
    g: dict[str, np.ndarray],
    res: int,
    pad_sigma: float,
    support_sigma: float,
) -> tuple[np.ndarray, np.ndarray, np.ndarray, float]:
    """Rasterize sum_i opacity_i * exp(-0.5 * mahalanobis^2) onto a dense grid.

    Returns volume (Z,Y,X) for skimage, origin (xyz of voxel 0), voxel size, max value.
    """
    pos = g["pos"]
    opacity = g["opacity"]
    scale = g["scale"]
    R = quat_to_rotmats(g["quat"])

    # Conservative world AABB: mean +/- pad_sigma * max axis scale
    extent = pad_sigma * scale.max(axis=1, keepdims=True)
    bmin = (pos - extent).min(axis=0)
    bmax = (pos + extent).max(axis=0)
    # Avoid zero-volume
    span = np.maximum(bmax - bmin, 1e-6)
    bmin = bmin - 0.02 * span
    bmax = bmax + 0.02 * span
    span = bmax - bmin

    # Isotropic voxel size from longest axis
    voxel = float(span.max() / max(res - 1, 1))
    dims = np.maximum(2, np.ceil(span / voxel).astype(np.int32) + 1)
    # Cap memory: soft limit ~192^3
    while int(dims.prod()) > 192 ** 3:
        voxel *= 1.15
        dims = np.maximum(2, np.ceil(span / voxel).astype(np.int32) + 1)

    nx, ny, nz = int(dims[0]), int(dims[1]), int(dims[2])
    print(f"grid {nx}x{ny}x{nz} voxel={voxel:.5f}  splats={pos.shape[0]}", flush=True)

    vol = np.zeros((nz, ny, nx), dtype=np.float32)  # Z,Y,X for marching_cubes
    inv_voxel = 1.0 / voxel
    origin = bmin.copy()

    # Precompute inverse scale (diagonal in local frame)
    inv_s = 1.0 / np.maximum(scale, 1e-8)

    # For each splat, stamp local box
    half = support_sigma  # in sigma units along each axis
    for i in range(pos.shape[0]):
        # Local AABB in world: R * (+/- half * s)
        corners = []
        for sx in (-half, half):
            for sy in (-half, half):
                for sz in (-half, half):
                    local = np.array([sx * scale[i, 0], sy * scale[i, 1], sz * scale[i, 2]])
                    corners.append(pos[i] + R[i] @ local)
        corners = np.stack(corners, axis=0)
        cmin = corners.min(axis=0)
        cmax = corners.max(axis=0)

        i0 = max(0, int(math.floor((cmin[0] - origin[0]) * inv_voxel)))
        j0 = max(0, int(math.floor((cmin[1] - origin[1]) * inv_voxel)))
        k0 = max(0, int(math.floor((cmin[2] - origin[2]) * inv_voxel)))
        i1 = min(nx - 1, int(math.ceil((cmax[0] - origin[0]) * inv_voxel)))
        j1 = min(ny - 1, int(math.ceil((cmax[1] - origin[1]) * inv_voxel)))
        k1 = min(nz - 1, int(math.ceil((cmax[2] - origin[2]) * inv_voxel)))
        if i0 > i1 or j0 > j1 or k0 > k1:
            continue

        xs = origin[0] + np.arange(i0, i1 + 1, dtype=np.float64) * voxel
        ys = origin[1] + np.arange(j0, j1 + 1, dtype=np.float64) * voxel
        zs = origin[2] + np.arange(k0, k1 + 1, dtype=np.float64) * voxel
        # Meshgrid in X,Y,Z then convert offsets to local via R^T
        xx, yy, zz = np.meshgrid(xs, ys, zs, indexing="ij")
        # shape (nx',ny',nz',3)
        pts = np.stack([xx, yy, zz], axis=-1) - pos[i]
        # local = R^T * (p - mean)
        Rt = R[i].T
        loc = pts @ Rt.T  # (... ,3)
        m2 = (loc[..., 0] * inv_s[i, 0]) ** 2 + (loc[..., 1] * inv_s[i, 1]) ** 2 + (
            loc[..., 2] * inv_s[i, 2]
        ) ** 2
        w = opacity[i] * np.exp(-0.5 * m2)
        # vol is Z,Y,X — pts indexed i=x, j=y, k=z
        # w shape (nx', ny', nz')
        vol[k0 : k1 + 1, j0 : j1 + 1, i0 : i1 + 1] += w.transpose(2, 1, 0).astype(np.float32)

        if (i + 1) % 2000 == 0 or i + 1 == pos.shape[0]:
            print(f"  stamped {i + 1}/{pos.shape[0]}", flush=True)

    vmax = float(vol.max())
    print(f"opacity field max={vmax:.4f}", flush=True)
    return vol, origin, np.array([voxel, voxel, voxel], dtype=np.float64), vmax


def extract_mesh(
    vol: np.ndarray,
    origin: np.ndarray,
    spacing: np.ndarray,
    iso: float,
) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    if iso <= 0 or iso >= float(vol.max()):
        # Auto: fraction of max
        iso = 0.15 * float(vol.max())
        print(f"iso clamped/auto -> {iso:.5f}", flush=True)
    verts, faces, normals, _ = marching_cubes(vol, level=iso, spacing=tuple(spacing))
    # skimage returns verts in (z,y,x) order relative to array axes
    # With spacing, verts[:,0] is Z, [:,1] Y, [:,2] X
    xyz = np.empty_like(verts)
    xyz[:, 0] = origin[0] + verts[:, 2]
    xyz[:, 1] = origin[1] + verts[:, 1]
    xyz[:, 2] = origin[2] + verts[:, 0]
    # normals similarly z,y,x -> x,y,z
    nxyz = np.empty_like(normals)
    nxyz[:, 0] = normals[:, 2]
    nxyz[:, 1] = normals[:, 1]
    nxyz[:, 2] = normals[:, 0]
    # Flip to outward if needed: prefer normals pointing away from centroid
    centroid = xyz.mean(axis=0)
    flip = ((xyz - centroid) * nxyz).sum(axis=1) < 0
    nxyz[flip] *= -1.0
    # If majority flipped faces winding: when we flip normal, also swap face verts
    # Simpler: if mean radial·n < 0, flip all faces
    if ((xyz - centroid) * nxyz).sum() < 0:
        faces = faces[:, ::-1]
        nxyz *= -1.0
    return xyz.astype(np.float64), faces.astype(np.int32), nxyz.astype(np.float64)


def colour_vertices(
    verts: np.ndarray,
    g: dict[str, np.ndarray],
    k: int = 8,
) -> np.ndarray:
    """Opacity-weighted kNN colour from splat DC."""
    from scipy.spatial import cKDTree

    tree = cKDTree(g["pos"])
    d, ix = tree.query(verts, k=min(k, g["pos"].shape[0]))
    if ix.ndim == 1:
        return g["rgb"][ix]
    d = np.maximum(d, 1e-8)
    w = (1.0 / d) * g["opacity"][ix]
    w /= w.sum(axis=1, keepdims=True)
    rgb = (w[..., None] * g["rgb"][ix]).sum(axis=1)
    return np.clip(rgb, 0.0, 1.0)


def write_obj(
    path: Path,
    verts: np.ndarray,
    faces: np.ndarray,
    normals: np.ndarray,
    colours: np.ndarray | None,
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    mtl_name = path.with_suffix(".mtl").name
    with path.open("w", encoding="ascii") as f:
        f.write("# opacity-field mesh from 3DGS PLY\n")
        if colours is not None:
            f.write(f"mtllib {mtl_name}\n")
            f.write("usemtl splat_surface\n")
        for i, v in enumerate(verts):
            if colours is not None:
                c = colours[i]
                f.write(f"v {v[0]:.6f} {v[1]:.6f} {v[2]:.6f} {c[0]:.4f} {c[1]:.4f} {c[2]:.4f}\n")
            else:
                f.write(f"v {v[0]:.6f} {v[1]:.6f} {v[2]:.6f}\n")
        for n in normals:
            f.write(f"vn {n[0]:.6f} {n[1]:.6f} {n[2]:.6f}\n")
        for tri in faces:
            # 1-based indices; pair v//vn
            a, b, c = tri[0] + 1, tri[1] + 1, tri[2] + 1
            f.write(f"f {a}//{a} {b}//{b} {c}//{c}\n")
    if colours is not None:
        mean = colours.mean(axis=0)
        with path.with_suffix(".mtl").open("w", encoding="ascii") as f:
            f.write("newmtl splat_surface\n")
            f.write(f"Kd {mean[0]:.4f} {mean[1]:.4f} {mean[2]:.4f}\n")
            f.write("Ka 0.05 0.05 0.05\n")
            f.write("Ks 0.15 0.15 0.15\n")
            f.write("Ns 40\n")
    print(f"wrote {path}  verts={verts.shape[0]} faces={faces.shape[0]}", flush=True)


def write_pov_demo(path: Path, obj_rel: str, look_at: np.ndarray, radius: float) -> None:
    cam = look_at + np.array([radius * 0.85, radius * 0.35, -radius * 0.95])
    path.parent.mkdir(parents=True, exist_ok=True)
    text = f"""// Auto-generated surface mesh from splat opacity field
#version 3.8;
global_settings {{ assumed_gamma 1.0 }}

camera {{
  location <{cam[0]:.4f}, {cam[1]:.4f}, {cam[2]:.4f}>
  look_at <{look_at[0]:.4f}, {look_at[1]:.4f}, {look_at[2]:.4f}>
  angle 40
}}

background {{ color rgb <0.12, 0.14, 0.18> }}
light_source {{ <{cam[0]+1:.4f}, {cam[1]+2:.4f}, {cam[2]-1:.4f}> color rgb 1.3 }}
light_source {{ <-1, 1, -2> color rgb <0.3, 0.35, 0.45> }}

assimp {{
  "{obj_rel}"
  texture {{
    pigment {{ color rgb <0.75, 0.55, 0.45> }}
    finish {{ ambient 0.08 diffuse 0.75 specular 0.2 roughness 0.1 }}
  }}
}}
"""
    path.write_text(text, encoding="ascii")
    print(f"wrote {path}", flush=True)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ply", type=Path, help="3DGS Gaussian splat PLY")
    ap.add_argument("-o", "--output", type=Path, required=True, help="Output .obj path")
    ap.add_argument("--res", type=int, default=128, help="Grid resolution along longest axis")
    ap.add_argument("--iso", type=float, default=0.0,
                    help="Isosurface level (0 = auto 0.15*max)")
    ap.add_argument("--opacity-min", type=float, default=0.05, help="Drop low-opacity Gaussians")
    ap.add_argument("--max-scale", type=float, default=0.0,
                    help="Drop Gaussians with max axis scale above this (0 = off)")
    ap.add_argument("--pad-sigma", type=float, default=3.0, help="AABB padding in sigma")
    ap.add_argument("--support-sigma", type=float, default=2.5, help="Stamp radius in sigma")
    ap.add_argument("--pov", type=Path, default=None, help="Also write a POV demo scene")
    ap.add_argument("--no-colour", action="store_true", help="Skip vertex colours")
    args = ap.parse_args(argv)

    g = read_gaussians_ply(args.ply)
    print(f"loaded {g['pos'].shape[0]} gaussians from {args.ply}", flush=True)
    max_scale = args.max_scale if args.max_scale > 0 else None
    # Heuristic floater cull: drop very large scales relative to median
    if max_scale is None:
        med = float(np.median(g["scale"].max(axis=1)))
        max_scale = max(med * 12.0, 1e-4)
        print(f"auto max-scale={max_scale:.5f} (12x median)", flush=True)
    g = filter_splats(g, args.opacity_min, max_scale)
    print(f"kept {g['pos'].shape[0]} after filter", flush=True)

    vol, origin, spacing, vmax = build_opacity_grid(
        g, res=args.res, pad_sigma=args.pad_sigma, support_sigma=args.support_sigma
    )
    iso = args.iso if args.iso > 0 else 0.15 * vmax
    verts, faces, normals = extract_mesh(vol, origin, spacing, iso)
    colours = None if args.no_colour else colour_vertices(verts, g)
    write_obj(args.output, verts, faces, normals, colours)

    if args.pov is not None:
        obj_rel = Path(os.path.relpath(args.output, args.pov.parent)).as_posix()
        look = verts.mean(axis=0)
        radius = float(np.linalg.norm(verts.max(axis=0) - verts.min(axis=0)) * 0.75)
        write_pov_demo(args.pov, obj_rel, look, max(radius, 0.5))

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
