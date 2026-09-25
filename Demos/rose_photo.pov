// Offline GaussianSplatCloud — Vol3DGS analytic volume α (samples >= 3).
// samples 1 = 3D peak; 2 = 2D EWA; >=3 = ray-integrated ellipsoid opacity.
// Still needs a training-view camera for photoreal SH; without it colours stay wrong.
#version 3.8;
global_settings { assumed_gamma 1.0 }

camera {
  location <2.0851, 0.1118, -10.5769>
  look_at <-1.7874, -2.6543, 0.4874>
  angle 40
}

background { color rgb 0.0 }

gaussian_splat {
  "models/Rose.ply"
  sh_degree 3
  samples 4              // >=3 → Vol3DGS volume α
  alpha_stop 0.999
  opacity_cutoff 0.0039  // ~1/255
}

// From Demos/:
//   ../unix/povray +W640 +H640 +A0.3 +AM2 +R2 -D -Oresults/rose_vol.png rose_photo.pov
