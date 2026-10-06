// Training-view Gaussian splat scene (emission-only; SH is view-baked).
#version 3.8;
global_settings { assumed_gamma 1.0 }

// Auto-generated training camera from rose_aabb_c2w.json frame 0
// Do not edit by hand — regenerate with transforms_to_pov_camera.py
// Reference image (if present): reference/MISSING_put_training_frame_here.png
camera {
  location <2.08510000, 0.11180000, -10.57690000>
  sky <-0.07586806, 0.97327101, 0.21676617>
  look_at <-1.26267567, -2.27949303, -1.01181432>
  angle 40.000000
}

background { color rgb 1.0 }

gaussian_splat {
  "models/Rose.ply"
  sh_degree 3
  samples 2
  alpha_stop 0.999
  opacity_cutoff 0.0039
}
