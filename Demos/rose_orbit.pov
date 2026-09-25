// Rose turntable: camera orbits look_at, clock 0..3 = three revolutions.
// Starting pose matches rose_photo.pov. Drive with rose_orbit.ini.
#version 3.8;
global_settings { assumed_gamma 1.0 }

#declare LookAt      = <-1.82, -2.85, 0.52>;
#declare OrbitRadius = 7.992;
#declare OrbitHeight = LookAt.y + 1.35;  // photo camera height
#declare AngleDeg    = 45.0;
// Phase so clock=0 ≈ rose_photo location <2.0, -1.5, -6.5>
#declare Ang0 = atan2(2.0 - LookAt.x, -6.5 - LookAt.z);
#declare Ang  = Ang0 + clock * 2.0 * pi;

camera {
  location <LookAt.x + OrbitRadius * sin(Ang), OrbitHeight, LookAt.z + OrbitRadius * cos(Ang)>
  look_at LookAt
  angle AngleDeg
}

background { color rgb 0.02 }

gaussian_splat {
  "models/Rose.ply"
  sh_degree 3
  samples 2
  alpha_stop 0.999
  opacity_cutoff 0.05
}
