// Auto-generated surface mesh from splat opacity field
#version 3.8;
global_settings { assumed_gamma 1.0 }

camera {
  location <1.7550, -0.6658, -3.5505>
  look_at <-1.8178, -2.1370, 0.4426>
  angle 40
}

background { color rgb <0.12, 0.14, 0.18> }
light_source { <2.7550, 1.3342, -4.5505> color rgb 1.3 }
light_source { <-1, 1, -2> color rgb <0.3, 0.35, 0.45> }

assimp {
  "generated/rose_surface.obj"
  texture {
    pigment { color rgb <0.75, 0.55, 0.45> }
    finish { ambient 0.08 diffuse 0.75 specular 0.2 roughness 0.1 }
  }
  // test: upright Y-up - rotate 180 about Z about AABB center
  translate <1.7874, 2.6543, -0.4874>
  rotate <0, 0, 180>
  translate <-1.7874, -2.6543, 0.4874>
}
