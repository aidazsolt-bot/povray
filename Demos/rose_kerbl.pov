// Kerbl Jacobian EWA (samples 2) — screen-space Σ'=J W Σ Wᵀ Jᵀ + 0.3px low-pass.
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
  samples 2
  alpha_stop 0.999
  opacity_cutoff 0.0039
}

// ../unix/povray +W640 +H640 +A0.3 +AM2 +R2 -D -Oresults/rose_kerbl.png rose_kerbl.pov
