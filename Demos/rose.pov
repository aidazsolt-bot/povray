// PLY Gaussian-splat import demo — Rose.ply
// Uses GaussianSplatCloud (soft BVH + SH alpha composite). For emission-only
// photoreal-style lighting see rose_photo.pov / rose_training_camera.pov.
// Camera framed from Assimp AABB of the same model.
#version 3.8;
global_settings { assumed_gamma 1.0 }

camera {
  location <2.0851, 0.1118, -10.5769>
  look_at <-1.7874, -2.6543, 0.4874>
  angle 40
}

light_source { <-10.6388, 9.5165, -9.4705> color rgb 1.2 }
background { color rgb 0.02 }

ply { "models/Rose.ply" }
