# Assimp + Vulkan Gaussian splat build (VS 2022)

Stock `Release|x64` builds leave Assimp/Vulkan **off** (CI-safe stubs).
Enabling these dependencies also selects C++17, as required by Assimp, and
retains the legacy standard library facilities needed by the bundled Boost.

To enable on your machine:

1. Open [`gsplat_deps.props`](gsplat_deps.props) in an editor.
2. Set `PovEnableGsplatDeps` to `true`.
3. Point the Assimp / Vulkan paths at your trees (comments in the props file).
4. Open `povray.sln`, rebuild **GUI** or **console** (`Release|x64`).
5. Copy the Assimp DLL next to the exe (`windows\vs2022\bin64\`).
6. Ensure a Vulkan ICD is installed (`vulkan-1.dll` from the driver/SDK).

Scene usage (Assimp is the only model import path in gsplat builds;
upstream experimental `mesh { obj ... }` stays off / default-dead):

```pov
assimp { "model.ply" }              // mesh and/or Gaussians (any Assimp format)
assimp { "model.obj" }              // meshes + materials (OBJ, glTF, MD5, ...)
gaussian_splat { "model.ply" ... }  // same Assimp pipeline, splat-oriented options
mesh { assimp "model.obj" }         // triangle mesh only
// inside gaussian_splat / assimp splat options:
backend vulkan   // opt-in; default is cpu
// backend cpu
```

### `backend` (CPU vs Vulkan)

- **Default is `cpu`.** Omit `backend` or set `backend cpu` for the CPU
  projector. Vulkan is **opt-in** via `backend vulkan`.
- **Commenting out:** use POV `//` (or `/* ... */`), not `#`. A leading `#`
  starts a language directive (`#declare`, `#version`, ...), so
  `#backend vulkan` does **not** disable the option and is not a comment.
  To leave the line in the file unused: `// backend vulkan`.
- **Fallback:** `backend vulkan` falls back to CPU if device init fails.
  Dispatch failures (for example out-of-memory on large resolutions) also
  fall back, with a diagnostic on stderr. A transparent pixel in a
  successfully rendered GPU frame is a miss, not a CPU fallback.
- **Speed:** Vulkan only accelerates the primary-hit splat framebuffer
  bake (project / tile / blend). The rest of POV-Ray (rays, secondary
  integration, materials) stays on the CPU. Large SSBOs (framebuffer,
  projections, tile lists) use `DEVICE_LOCAL` VRAM with a host staging
  buffer for upload/readback; successful frames log `mem=device_local`.
  A fair MinGW console A/B on `Demos/apple_orbit.pov` vs
  `apple_orbit-nv.pov` (`+W1024 +H800 -D`, ~36k splats, 2 threads,
  NVIDIA GeForce MX150) measured approximately:
  - `backend vulkan`: bake `host_gpu≈284ms` / `gpu_ts≈68ms`
    (`mem=device_local`), Trace ≈50s, wall ≈57s
  - `backend cpu`: Trace ≈20s, wall ≈48s
  So on that laptop the bake is fast on GPU, but end-to-end Trace/wall
  still favors CPU. Paths are not pixel-identical (different ray /
  transmission counts). Re-measure on your GPU; look for
  `gaussian_splat vulkan frame: ... host_gpu=...`. The Support Libraries
  banner listing Vulkan only means the binary was built with the option,
  not that a given scene uses it.

Camera framebuffers are prepared on the first primary intersection and shared
read-only by the render threads; changing the camera or image size selects a
new snapshot. Only primary projected splats use this framebuffer. Reflection,
transmission and other secondary rays retain their directional CPU integration
and scene material settings, so Vulkan does not replace the complete POV-Ray
renderer. Secondary integration reuses object-space covariance/normal data
built with the BVH and constructs its projection basis once per ray, without
changing opacity, support, sorting or material parameters.

Windows integration regression tests (requires a working Vulkan device):

```powershell
.\tests\render\test_gaussian_vulkan.ps1 -Renderer .\windows\vs2022\bin64\povconsole64.exe
```

An optional `-ReferenceRenderer` compares CPU relight output, including secondary
rays and sample modes 1, 2 and 4, pixel-for-pixel against a previous executable.

Shader SPIR-V is already embedded under `source/core/shape/shaders/*_spv.inc`.
Regenerate only if you edit the `.comp` files:

```bash
glslc=... python3 source/core/shape/shaders/embed_spv.py
```
