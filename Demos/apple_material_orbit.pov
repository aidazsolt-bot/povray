// Apple turntable with POV material shading (diffuse/specular), like rose_material.
// Floor on y=0; no walls. Drive with apple_material_orbit.ini.
// 180 frames @ 10 fps → 18 s, clock 0..3 = three revolutions.
#version 3.8;
global_settings { assumed_gamma 1.0 }

#declare OrbitRadius = 1.00;
#declare OrbitHeight = 0.35;
#declare LookAt     = <0.0, 0.02, 0.0>;
#declare AngleDeg   = 45.0;
#declare Ang = clock * 2.0 * pi;

camera {
  location <OrbitRadius * sin(Ang), OrbitHeight, OrbitRadius * cos(Ang)>
  look_at LookAt
  angle AngleDeg
}

background { color rgb <0.12, 0.14, 0.18> }

light_source { <1.5, 2.5, -1.0> color rgb 1.4 }
light_source { <-1.0, 0.8, -1.5> color rgb <0.35, 0.4, 0.55> }

// Single control plane: floor on the y=0 plane (no walls).
plane {
  y, 0
  pigment { checker color rgb 0.12 color rgb 0.55 scale 0.15 }
  finish { ambient 0.25 diffuse 0.7 }
}

gaussian_splat {
  "public/apple/splats/apple.ply"
  sh_degree 3
  samples 2
  alpha_stop 0.999
  opacity_cutoff 0.0039
  gi_weight 0
  texture {
    pigment { color rgb 1 } // overwritten per hit by splat SH
    finish {
      ambient 0.05
      diffuse 0.75
      specular 0.25
      roughness 0.08
      emission rgb 0.12
    }
  }
  rotate <180, 0, 0>  // test: upright Y-up splat (OpenGL/COLMAP often inverted)
}
