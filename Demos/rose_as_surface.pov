// SuperSplat-style as_surface (Rose): floaters + optional smooth MC, POV EvalColour.
#version 3.8;
global_settings { assumed_gamma 1.0 }

camera {
  location <2.0, -1.5, -6.5>
  look_at <-1.82, -2.85, 0.52>
  angle 45
}

background { color rgb <0.12, 0.14, 0.18> }
light_source { <5, 8, -4> color rgb 1.4 }
light_source { <-3, 2, -8> color rgb <0.35, 0.4, 0.55> }

gaussian_splat {
  "models/Rose.ply"
  as_surface
  filter_floaters        // SuperSplat defaults: 0.05, 0.1, 0.004
  surface_smooth         // marching cubes (omit for voxel faces)
  // voxel_size 0.05
  // voxel_opacity 0.1
  sh_degree 3
}

// ../unix/povray +W800 +H600 -D -Oresults/rose_as_surface.png rose_as_surface.pov
