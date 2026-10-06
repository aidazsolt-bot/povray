// Training-view Gaussian splat scene (emission-only; SH is view-baked).
#version 3.8;
global_settings { assumed_gamma 1.0 }

// Auto-generated training camera from apple_transforms.json frame 90
// Do not edit by hand — regenerate with transforms_to_pov_camera.py
// Reference image (if present): images/frame_00090.png
camera {
  location <-0.55708986, 0.26239343, -0.57084656>
  sky <0.05371726, 0.99703793, 0.05504374>
  look_at <0.01075792, 0.19967612, 0.01102357>
  angle 45.000000
}

background { color rgb 1.0 }

gaussian_splat {
  "public/apple/splats/apple.ply"
  sh_degree 3
  samples 2
  alpha_stop 0.999
  opacity_cutoff 0.0039
}
