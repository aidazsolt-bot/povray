// PLY mesh import demo — Wuson.ply
// Camera framed from Assimp AABB of the same model.
#version 3.8;
global_settings { assumed_gamma 1.0 }

camera {
  location <2.3999, 2.4716, -6.8570>
  look_at <0.0000, 0.7573, 0.0000>
  angle 40
}

light_source { <-5.4856, 8.3000, -6.1713> color rgb 1.2 }
background { color rgb <0.2, 0.3, 0.4> }

ply { "models/Wuson.ply"
  pigment { rgb <0.85, 0.75, 0.65> }
  finish { ambient 0.25 diffuse 0.75 }
}
