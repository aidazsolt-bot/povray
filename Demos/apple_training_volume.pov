// Training-view Gaussian splat scene (emission-only; SH is view-baked).
#version 3.8;
global_settings { assumed_gamma 1.0 }

// Auto-generated training camera from apple_transforms.json frame 0
// Do not edit by hand — regenerate with transforms_to_pov_camera.py
// Reference image (if present): images/frame_00000.png
camera {
  location <-0.00000000, 1.00074292, 0.01396193>
  sky <0.00000000, 0.01745241, -0.99984770>
  look_at <-0.00000000, 0.00006118, -0.00350503>
  angle 45.000000
}

background { color rgb 1.0 }

gaussian_splat {
  "public/apple/splats/apple.ply"
  sh_degree 3
  samples 4
  alpha_stop 0.999
  opacity_cutoff 0.0039
}
