# Assimp + Vulkan Gaussian splat build (VS 2022)

Stock `Release|x64` builds leave Assimp/Vulkan **off** (CI-safe stubs).

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

Shader SPIR-V is already embedded under `source/core/shape/shaders/*_spv.inc`.
Regenerate only if you edit the `.comp` files:

```bash
glslc=... python3 source/core/shape/shaders/embed_spv.py
```
