// SuperSplat-style as_surface (Apple): floaters + smooth MC, POV EvalColour.
#version 3.8;
global_settings { assumed_gamma 1.0 }

camera {
  location <1.0, 0.35, -1.0>
  look_at <0.0, 0.02, 0.0>
  angle 45
}

background { color rgb <0.12, 0.14, 0.18> }
light_source { <1.5, 2.5, -1.0> color rgb 1.4 }
light_source { <-1.0, 0.8, -1.5> color rgb <0.35, 0.4, 0.55> }

gaussian_splat {
  "public/apple/splats/apple.ply"
  as_surface
  filter_floaters 0.05, 0.1, 0.004
  surface_smooth
  sh_degree 3
}

// ../unix/povray +W800 +H600 -D -Oresults/apple_as_surface.png apple_as_surface.pov
