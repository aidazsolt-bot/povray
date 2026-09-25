// Apple turntable: camera orbits 360° around the splatfacto PLY.
// Drive with apple_orbit.ini (clock 0..1 = full revolution).
// PLY frame matches DX.GL Apple with training cameras at ~1 unit radius
// (--position-scale 0.2 relative to raw transforms).
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

background { color rgb 1.0 }

gaussian_splat {
  "public/apple/splats/apple.ply"
  sh_degree 3
  samples 2              // Kerbl EWA
  alpha_stop 0.999
  opacity_cutoff 0.0039
}
