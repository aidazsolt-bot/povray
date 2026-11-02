// Rose.ply - KHR ellipse + lighting_mode khr + sh_color_space srgb_rec709_display.
// Camera framing matches rose.pov (Assimp AABB).
#version 3.8;
global_settings { assumed_gamma 1.0 }

camera {
  location <2.0851, 0.1118, -10.5769>
  look_at <-1.7874, -2.6543, 0.4874>
  angle 40
}

light_source { <-10.6388, 9.5165, -9.4705> color rgb 1.2 }
background { color rgb 0.02 }

gaussian_splat {
  "models/Rose.ply"
  khr_ellipse
  sort_depth
  backend vulkan
  lighting_mode khr
  sh_degree 3
  sh_color_space srgb_rec709_display
  // upright Y-up - rotate 180 about Z about AABB center (same as rose.pov)
  translate <1.7874, 2.6543, -0.4874>
  rotate <0, 0, 180>
  translate <-1.7874, -2.6543, 0.4874>
}
