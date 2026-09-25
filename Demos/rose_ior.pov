// Veiled-baseline Gaussians + material/IOR experiment
// Experiment: splat pigment + glass-ish finish + interior IOR (refraction).
#version 3.8;
global_settings { assumed_gamma 1.0 }

camera {
  location <2.0, -1.5, -6.5>
  look_at <-1.82, -2.85, 0.52>
  angle 45
}

background { color rgb <0.12, 0.14, 0.18> }

light_source { <6, 10, -3> color rgb 1.6 }
light_source { <-4, 1, -6> color rgb <0.4, 0.45, 0.6> }

// Backdrop BEHIND the bouquet (subject ~ z=0.5, camera ~ z=-6.5)
plane {
  z, 3.5
  pigment { checker color rgb 0.12 color rgb 0.55 scale 0.8 }
  finish { ambient 0.25 diffuse 0.7 }
}

gaussian_splat {
  "models/Rose.ply"
  sh_degree 3
  samples 2
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

// ../unix/povray +W960 +H540 -D -Oresults/rose_ior.png rose_ior.pov
