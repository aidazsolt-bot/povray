// Assimp animation demo — Fox.glb (clips reported; bind-pose mesh)
// Camera framed from Assimp AABB.
#version 3.8;
global_settings { assumed_gamma 1.0 }

camera {
  location <113.9486, 120.7846, -336.3025>
  look_at <0.0000, 39.3927, -10.7351>
  angle 40
}

light_source { <-260.4539, 397.5169, -303.7457> color rgb 1.25 }
background { color rgb <0.4, 0.55, 0.7> }

assimp { "models/Fox.glb" }
