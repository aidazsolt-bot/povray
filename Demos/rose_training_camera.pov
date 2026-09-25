// Stub: training-camera match for photoreal Gaussian splat reconstruction.
// Fill camera from COLMAP / transforms.json of the capture that produced the PLY,
// point gaussian_splat at that model, keep lights off (SH is emission-baked).
//
// Example workflow:
//   1) Export transforms.json + images from the 3DGS training run
//   2) Set location / look_at / angle (or right/up) to frame 0000
//   3) Render and visually compare against images/0000.png
#version 3.8;
global_settings { assumed_gamma 1.0 }

// TODO: replace with training-view extrinsics/intrinsics
camera {
  location <2.0851, 0.1118, -10.5769>
  look_at <-1.7874, -2.6543, 0.4874>
  angle 40
}

background { color rgb 0.0 }

gaussian_splat {
  "models/Rose.ply"
  sh_degree 3
}

// Optional: #include a generated camera block from tools/ once COLMAP data is wired.
