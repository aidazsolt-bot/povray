// OBJ import demo — Earth.obj
// MTL maps loaded automatically from Earth.mtl next to the model:
//   map_Kd  Earth_albedo.png     → pigment image_map (UV)
//   bump / map_bump Earth_bump.png → normal bump_map
//   map_Ks  Earth_specular.png   → finish specular/roughness (from Ks/Ns; POV has no specular_map)
//   norm / map_Kn Earth_normal.png → bump_map fallback if no height bump
// POV-controlled specular/reflection (no map_Ks): see earth_shine.pov
// Camera framed from Assimp AABB of the same model.
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

mesh {
  obj "models/Earth/Earth.obj"
}
