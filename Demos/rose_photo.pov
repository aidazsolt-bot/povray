// Veiled-baseline still: project/sort/blend + channel-major SH, soft falloff,
// low opacity cutoff, volume path still allowed (pre anti-floater hardening).
#version 3.8;
global_settings { assumed_gamma 1.0 }

camera {
  location <2.0, -1.5, -6.5>
  look_at <-1.82, -2.85, 0.52>
  angle 45
}

background { color rgb 0.02 }

gaussian_splat {
  "models/Rose.ply"
  sh_degree 3
  samples 2
  alpha_stop 0.999
  opacity_cutoff 0.0039
  // test: upright Y-up - rotate 180 about Z about AABB center
  translate <1.7874, 2.6543, -0.4874>
  rotate <0, 0, 180>
  translate <-1.7874, -2.6543, 0.4874>
}

// ../unix/povray +W1920 +H1080 -D -Oresults/rose_veiled_baseline.png rose_photo.pov
