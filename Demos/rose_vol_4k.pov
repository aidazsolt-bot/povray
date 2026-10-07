// Rose volume (ellipsoid in/out, samples 4) at 4K framing.
// Floor plane: 1 unit below AABB min.y of Rose.ply (no back wall).
#version 3.8;
global_settings { assumed_gamma 1.0 }

// Rose.ply Assimp AABB (positions): min.y ≈ -4.484253
#declare RoseAabbMinY = -4.484253;
#declare FloorY       = RoseAabbMinY - 1.0; // -5.484253
#declare LookAt       = <-1.787, -2.654, 0.487>; // AABB center

camera {
  location <2.0, -1.2, -7.0>
  look_at LookAt
  angle 45
}

background { color rgb <0.12, 0.14, 0.18> }

light_source { <6, 8, -4> color rgb 1.6 }
light_source { <-4, 0, -6> color rgb <0.4, 0.45, 0.6> }

// Floor under the bouquet (y-up), not a wall behind the subject.
plane {
  y, FloorY
  pigment { checker color rgb 0.12 color rgb 0.55 scale 0.8 }
  finish { ambient 0.25 diffuse 0.7 }
}

gaussian_splat {
  "models/Rose.ply"
  sh_degree 3
  samples 4
  opacity_cutoff 0.0039
}

// ../unix/povray +W3840 +H2160 -D -Oresults/rose_vol_4k.png rose_vol_4k.pov
