// Emission-only GaussianSplatCloud demo — Rose.ply
// Soft BVH + SH compositing (no scene lights; appearance is baked in SH).
// Camera framed from Assimp AABB of the same model.
#version 3.8;
global_settings { assumed_gamma 1.0 }

camera {
  location <2.0851, 0.1118, -10.5769>
  look_at <-1.7874, -2.6543, 0.4874>
  angle 40
}

background { color rgb 0.02 }

// Prefer dedicated SDL token; falls through Assimp splat → GaussianSplatCloud.
gaussian_splat {
  "models/Rose.ply"
  // max_count 50000   // optional throttle
  // sh_degree 3       // 0..3 (default 3)
}

// Legacy aliases still work:
//   ply { "models/Rose.ply" }
//   assimp { "models/Rose.ply" }
