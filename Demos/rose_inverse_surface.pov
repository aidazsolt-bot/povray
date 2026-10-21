// Inverse-surface extract: opaque MC mesh from Gaussian occupancy (no volume, no IOR).
// Floor is 1 unit below Rose.ply AABB min.y.
#version 3.8;
global_settings { assumed_gamma 1.0 }

#declare RoseAabbMinY = -4.484253;
#declare FloorY       = RoseAabbMinY - 1.0;
#declare LookAt       = <-1.787, -2.654, 0.487>;

camera {
  location <2.0, -1.2, -7.0>
  look_at LookAt
  angle 45
}

background { color rgb <0.12, 0.14, 0.18> }

light_source { <6, 8, -4> color rgb 1.6 }
light_source { <-4, 0, -6> color rgb <0.4, 0.45, 0.6> }

plane {
  y, FloorY
  pigment { checker color rgb 0.12 color rgb 0.55 scale 0.8 }
  finish { ambient 0.25 diffuse 0.7 }
}

gaussian_splat {
  "models/Rose.ply"
  inverse_surface
  export_mesh "generated/rose_inverse.obj"
  sh_degree 3
  // Optional: denser voxels for finer extract
  // voxel_size 0.04
  // voxel_opacity 0.08
  texture {
    pigment { color rgb 1 } // overwritten per triangle by SH
    finish {
      ambient 0.05
      diffuse 0.85
      specular 0.15
      roughness 0.08
    }
  }
  // test: upright Y-up - rotate 180 about Z about AABB center
  translate <1.7874, 2.6543, -0.4874>
  rotate <0, 0, 180>
  translate <-1.7874, -2.6543, 0.4874>
}

// ../unix/povray +W800 +H600 -D -Oresults/rose_inverse_surface.png rose_inverse_surface.pov
