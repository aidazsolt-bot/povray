// Training-view Gaussian splat scene (emission-only; SH is view-baked).
#version 3.8;
global_settings { assumed_gamma 1.0 }

// Auto-generated training camera from rose_aabb_c2w.json frame 0
// Do not edit by hand — regenerate with transforms_to_pov_camera.py
// Reference image (if present): reference/MISSING_put_training_frame_here.png
camera {
  location <2.08510000, 0.11180000, -10.57690000>
  direction <-0.32152012, -0.22965960, 0.91863011>
  right <0.68707279, -0.00000000, 0.24047517>
  up <-0.05522743, 0.70848336, 0.15779286>
  // angle 40.000000  // implied by |right|/|direction|; kept as comment
}

background { color rgb 0.0 }

gaussian_splat {
  "models/Rose.ply"
  sh_degree 3
  samples 4
  alpha_stop 0.999
  opacity_cutoff 0.0039
}
