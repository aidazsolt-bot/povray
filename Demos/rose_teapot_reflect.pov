// Rose.ply splat + chrome Utah teapot (advanced/teapot) for secondary-ray
// reflection check. Teapot sits clear of the rose AABB; camera frames both
// so the rose should appear in the chrome body.
#version 3.8;
global_settings {
  assumed_gamma 1.0
  max_trace_level 8
}

#include "colors.inc"
#include "textures.inc"
#include "metals.inc"

#declare RoseCenter   = <-1.7874, -2.6543, 0.4874>;
#declare RoseAabbMinY = -4.484253;
#declare FloorY       = RoseAabbMinY - 0.05;

// Teapot after Teapot_Orientation is ~3 units tall; scale and park on +X
// so it does not intersect the rose (Assimp AABB max.x ~= -0.22).
#declare TeapotScale  = 1.35;
// After <-110,20,0> the pot sits near y~0; nudge so the base rests on FloorY.
// A bit left (-X) and further from camera (+Z, deeper in frame).
#declare TeapotPos    = <3.4, FloorY + 2.35, 1.2>;

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
  "models/Rose.ply"
  sh_degree 3
  samples 2
  alpha_stop 0.995
  opacity_cutoff 0.005
  gi_weight 1.0
  // upright Y-up - rotate 180 about Z about AABB center
  translate <1.7874, 2.6543, -0.4874>
  rotate <0, 0, 180>
  translate <-1.7874, -2.6543, 0.4874>
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
