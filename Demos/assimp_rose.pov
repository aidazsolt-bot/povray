// Assimp import demo — Rose.ply Gaussian splats via aiGetGaussianSplat()
// Camera framed from Assimp AABB (splat vertex positions).
#version 3.8;
global_settings { assumed_gamma 1.0 }

camera {
  location <2.0851, 0.1118, -10.5769>
  look_at <-1.7874, -2.6543, 0.4874>
  angle 40
}

light_source { <-10.6388, 9.5165, -9.4705> color rgb 1.2 }
background { color rgb 0.02 }

assimp { "models/Rose.ply" }
