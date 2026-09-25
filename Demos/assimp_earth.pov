// Assimp textured globe — Earth.obj
// Assimp+MTL maps (same assets as native obj import):
//   diffuse/albedo → Earth_albedo.png
//   height/bump    → Earth_bump.png
//   specular       → Earth_specular.png (finish Ks/Ns; image noted)
//   normals        → Earth_normal.png (bump_map approx if no height map)
// Camera framed from Assimp AABB (unit sphere).
#version 3.8;
global_settings { assumed_gamma 1.0 }

camera {
  location <2.2485, 1.6061, -6.4243>
  look_at <0.0000, 0.0000, 0.0000>
  angle 40
}

light_source { <-5.1395, 7.0668, -5.7819> color rgb 1.35 }
light_source { <4, 1, 3> color rgb 0.25 }
background { color rgb <0.02, 0.02, 0.05> }

assimp { "models/Earth/Earth.obj" }
