// Offline GI / secondary-ray demo: Rose splat + mirror floor on y=0.
// Bounce rays that hit the floor still integrate the splat volume along the
// open segment (Trace hook); the mirror reflection hits the cloud directly.
// Rose.ply AABB min.y ≈ -4.48 — lift so the bouquet sits on the y=0 floor.
#version 3.8;
global_settings {
  assumed_gamma 1.0
  max_trace_level 8
}

#declare SplatYLift = 4.5;

camera {
  location <3.5, 1.2 + SplatYLift, -9.0>
  look_at <-0.5, -1.5 + SplatYLift, 0.5>
  angle 42
}

background { color rgb 0.01 }

light_source { <20, 40 + SplatYLift, -10> color rgb 0.35 }

gaussian_splat {
  "models/Rose.ply"
  sh_degree 3
  samples 4
  alpha_stop 0.995
  opacity_cutoff 0.005
  gi_weight 1.0
  translate <0, SplatYLift, 0>
}

// Single control plane: glossy floor on the y=0 plane (no walls).
plane {
  y, 0
  pigment { color rgb <0.08, 0.08, 0.1> }
  finish {
    ambient 0.05
    diffuse 0.15
    reflection { 0.65 metallic 0 }
  }
}

// Suggested:
//   ../unix/povray +W640 +H480 +A0.3 -D rose_gi_mirror.pov
// Compare with gi_weight 0 to isolate bounce-segment contribution.
