// Veiled-baseline Gaussians + material/IOR experiment
// Experiment: Gaussian splat as POV material (diffuse/specular) + light.
// Auto materialShading when finish has diffuse/specular.
#version 3.8;
global_settings { assumed_gamma 1.0 }

camera {
  location <2.0, -1.5, -6.5>
  look_at <-1.82, -2.85, 0.52>
  angle 45
}

background { color rgb 0.02 }

light_source { <5, 8, -4> color rgb 1.4 }
light_source { <-3, 2, -8> color rgb <0.35, 0.4, 0.55> }

gaussian_splat {
  "models/Rose.ply"
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
}

// ../unix/povray +W960 +H540 -D -Oresults/rose_material.png rose_material.pov
