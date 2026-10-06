// Apple static sample: material shading (like rose_material), alpha_stop 0.75.
#version 3.8;
global_settings { assumed_gamma 1.0 }

#declare OrbitRadius = 1.00;
#declare OrbitHeight = 0.35;
#declare LookAt     = <0.0, 0.02, 0.0>;
#declare AngleDeg   = 45.0;

camera {
  location <OrbitRadius * sin(0), OrbitHeight, OrbitRadius * cos(0)>
  look_at LookAt
  angle AngleDeg
}

background { color rgb <0.12, 0.14, 0.18> }

light_source { <1.5, 2.5, -1.0> color rgb 1.4 }
light_source { <-1.0, 0.8, -1.5> color rgb <0.35, 0.4, 0.55> }

plane {
  y, 0
  pigment { checker color rgb 0.12 color rgb 0.55 scale 0.15 }
  finish { ambient 0.25 diffuse 0.7 }
}

gaussian_splat {
  "public/apple/splats/apple.ply"
  sh_degree 3
  samples 2
  alpha_stop 0.75
  opacity_cutoff 0.0039
  gi_weight 0
  texture {
    pigment { color rgb 1 }
    finish {
      ambient 0.05
      diffuse 0.75
      specular 0.25
      roughness 0.08
      emission rgb 0.12
    }
  }
}

// ../unix/povray +W4096 +H2160 -D -Oresults/apple_material.png apple_material.pov
