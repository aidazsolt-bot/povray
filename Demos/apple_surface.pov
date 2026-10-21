// Auto-generated surface mesh from splat opacity field
#version 3.8;
global_settings { assumed_gamma 1.0 }

camera {
  location <0.5630, 0.2705, -0.6725>
  look_at <0.0171, 0.0457, -0.0624>
  angle 40
}

background { color rgb <0.12, 0.14, 0.18> }
light_source { <1.5630, 2.2705, -1.6725> color rgb 1.3 }
light_source { <-1, 1, -2> color rgb <0.3, 0.35, 0.45> }

assimp {
  "generated/apple_surface.obj"
  texture {
    pigment { color rgb <0.75, 0.55, 0.45> }
    finish { ambient 0.08 diffuse 0.75 specular 0.2 roughness 0.1 }
  }
  rotate <180, 0, 0>  // test: upright Y-up splat (OpenGL/COLMAP often inverted)
}
