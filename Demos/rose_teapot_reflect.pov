// InteriorDesign.ply splat + chrome Utah teapot for secondary-ray reflection.
// Teapot/camera/floor framed as before (rose-era FloorY); splat asset swapped.
#version 3.8;
global_settings {
  assumed_gamma 1.0
  max_trace_level 8
}

#include "colors.inc"
#include "textures.inc"
#include "metals.inc"

// Keep floor/teapot height from the prior rose framing (teapot/camera unchanged).
#declare RoseAabbMinY = -4.484253;
#declare FloorY       = RoseAabbMinY - 0.05;

#declare TeapotScale  = 1.35;
#declare TeapotPos    = <1.0, FloorY + 2.35, 1.2 + 4.0>;

#declare Flat_Enough = 0.0001;
#declare Teapot_Texture = T_Chrome_5E;
#declare Teapot_Orientation = <-110, 20, 0>;

camera {
  location <6.0, 0.2, -11.5>
  look_at <1.0, -2.0, 0.0>
  angle 40
}

background { color rgb <0.04, 0.045, 0.06> }

light_source { <8, 10, -6> color rgb 1.5 }
light_source { <-6, 4, -4> color rgb <0.35, 0.4, 0.55> }
light_source { <4, 2, 6> color rgb <0.25, 0.22, 0.2> }

plane {
  y, FloorY
  pigment { checker color rgb 0.1 color rgb 0.35 scale 1.2 }
  finish { ambient 0.15 diffuse 0.65 reflection 0.08 }
}

gaussian_splat {
  "models/InteriorDesign.ply"
  sh_degree 3
  // samples 2 OOMs/segfaults at >=~640px with this ~151k splat cloud.
  samples 1
  alpha_stop 0.995
  opacity_cutoff 0.005
  gi_weight 1.0
}

// teapot_inc_nodecl.inc is upstream teapot.inc without its Flat_Enough declare
// (declaring inside object{} is illegal and emptied the prior broken scene).
#declare ChromeTeapot = object {
  #include "teapot_inc_nodecl.inc"
  scale TeapotScale
  translate TeapotPos
}
object { ChromeTeapot }

// From Demos/:
//   ../build-host/unix/povray +W960 +H720 +A0.3 -D \
//     +L../distribution/include \
//     -Oresults/rose_teapot_reflect.png rose_teapot_reflect.pov
