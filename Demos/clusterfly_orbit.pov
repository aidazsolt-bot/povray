// ClusterFly_M.ply - 360 deg camera orbit (clock 0..1), KHR ellipse projection.
// KHR reference:
// https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Khronos/KHR_gaussian_splatting/README.md
// Model: Postshot 3DGS ~146k splats
// Encode: 160 frames @ 20 fps -> 8 s
#version 3.8;
global_settings {
  assumed_gamma 1.0
  ambient_light rgb 1
}

#declare AabbMin = <-0.09943, -0.16238, -0.11570>;
#declare AabbMax = < 0.16975,  0.01316,  0.10879>;
#declare LookAt  = < 0.03516, -0.07461, -0.00346>;
#declare OrbitRadius = 0.55;
#declare OrbitHeight = LookAt.y + 0.12;
#declare AngleDeg    = 45.0;
#declare Ang         = clock * 2.0 * pi;

camera {
  location <LookAt.x + OrbitRadius * sin(Ang), OrbitHeight,
            LookAt.z + OrbitRadius * cos(Ang)>
  look_at LookAt
  angle AngleDeg
}

background { color rgb <0.06, 0.07, 0.09> }

// RenderAssimp desktop Unified key: d=<-0.45,-0.70,0.55>, shader L=-d.
#declare KeyL = vnormalize(<0.45, 0.70, -0.55>);
light_source {
  KeyL * 1000
  color rgb 1
  parallel
  point_at LookAt
  shadowless
}
light_source {
  -KeyL * 1000
  color rgb 1
  parallel
  point_at LookAt
  shadowless
}

gaussian_splat {
  "models/ClusterFlySplat/ClusterFly_M.ply"
  khr_ellipse
  sort_depth
  sh_degree 3
  alpha_clamp 99/100  // Per-splat alpha maximum; default 0.99, range 0..1.
  sigma_extent 3/1    // Gaussian support radius in sigma; default 3, >= 0.
  max_quad_frac 1/2   // Max radius / min(image size); KHR default 0.5, 0 disables.
  ewa_filter 3/10     // Projected covariance diagonal filter; default 0.3, >= 0.
  texture {
    pigment { color rgb 1 }
    finish {
      ambient 0.18
      emission 0
      diffuse 0.82
      brilliance 1
      specular 0
      phong 0
    }
  }
  // Postshot/COLMAP Y-down -> POV Y-up: 180 about X around AABB center
  translate <-LookAt.x, -LookAt.y, -LookAt.z>
  rotate <180, 0, 0>
  translate <LookAt.x, LookAt.y, LookAt.z>
}
