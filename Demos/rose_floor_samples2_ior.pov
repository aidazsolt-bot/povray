// Same floor framing as inverse_surface, but samples 2 (2D EWA) + IOR material.
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
  samples 2
  sh_degree 3
  alpha_stop 0.999
  opacity_cutoff 0.0039
  gi_weight 0
  texture {
    pigment { color rgb 1 }
    finish {
      ambient 0.02
      diffuse 0.35
      specular 0.55
      roughness 0.02
      reflection { 0.12 metallic 0.15 }
      emission rgb 0.1
    }
  }
  interior {
    ior 1.33
  }
}

// ../unix/povray +W3840 +H2160 -D -Oresults/rose_floor_samples2_ior.png rose_floor_samples2_ior.pov
