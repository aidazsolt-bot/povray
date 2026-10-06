// Veiled-baseline + IOR material, camera orbit (clock 0..3 = three revolutions).
// Half the angular speed of rose_orbit: same 180 frames, encode @ 10 fps → 18 s.
// Rose.ply AABB min.y ≈ -4.48 — lift so the bouquet sits on the y=0 floor.
#version 3.8;
global_settings { assumed_gamma 1.0 }

#declare SplatYLift = 4.5;
#declare LookAt      = <-1.82, -2.85 + SplatYLift, 0.52>;
#declare OrbitRadius = 7.992;
#declare OrbitHeight = LookAt.y + 1.35;
#declare AngleDeg    = 45.0;
#declare Ang0 = atan2(2.0 - LookAt.x, -6.5 - LookAt.z);
#declare Ang  = Ang0 + clock * 2.0 * pi;

camera {
  location <LookAt.x + OrbitRadius * sin(Ang), OrbitHeight, LookAt.z + OrbitRadius * cos(Ang)>
  look_at LookAt
  angle AngleDeg
}

background { color rgb <0.12, 0.14, 0.18> }

light_source { <6, 10 + SplatYLift, -3> color rgb 1.6 }
light_source { <-4, 1 + SplatYLift, -6> color rgb <0.4, 0.45, 0.6> }

// Single control plane: floor on the y=0 plane (no walls).
plane {
  y, 0
  pigment { checker color rgb 0.12 color rgb 0.55 scale 0.8 }
  finish { ambient 0.25 diffuse 0.7 }
}

gaussian_splat {
  "models/Rose.ply"
  khr_ellipse
  sh_degree 3
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
  translate <0, SplatYLift, 0>
}
