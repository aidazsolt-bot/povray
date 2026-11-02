// Rose.ply - same as rose_khr.pov but no light_source (KHR SH / emission only).
#version 3.8;
global_settings { assumed_gamma 1.0 }

camera {
  location <2.0851, 0.1118, -10.5769>
  look_at <-1.7874, -2.6543, 0.4874>
  angle 40
}

background { color rgb 0.02 }

gaussian_splat {
  "models/Rose.ply"
  khr_ellipse
  sort_depth
  backend vulkan
  lighting_mode khr
  sh_degree 3
  sh_color_space srgb_rec709_display
  translate <1.7874, 2.6543, -0.4874>
  rotate <0, 0, 180>
  translate <-1.7874, -2.6543, 0.4874>
}
