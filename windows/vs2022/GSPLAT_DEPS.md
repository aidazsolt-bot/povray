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

Scene usage:

```pov
assimp { "model.ply" }   // or .obj / other Assimp formats
// inside gaussian_splat options:
backend vulkan
```

`backend vulkan` falls back to CPU if device init fails.
Dispatch failures also fall back, with a diagnostic on stderr. A transparent
pixel in a successfully rendered GPU frame is a miss, not a CPU fallback.
Camera framebuffers are prepared on the first primary intersection and shared read-only
by the render threads; changing the camera or image size selects a new snapshot.
Only primary projected splats use this framebuffer. Reflection, transmission
and other secondary rays retain their directional CPU integration and scene
material settings, so Vulkan does not replace the complete POV-Ray renderer.
Secondary integration reuses object-space covariance/normal data built with
the BVH and constructs its projection basis once per ray, without changing
opacity, support, sorting or material parameters.

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
