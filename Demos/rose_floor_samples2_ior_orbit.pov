// samples 2 (2D EWA) + IOR, floor at AABB.min.y-1; camera 360 deg orbit (clock 0..1).
// Encode: 160 frames @ 20 fps -> 8 s.
#version 3.8;
global_settings { assumed_gamma 1.0 }

#declare RoseAabbMinY = -4.484253;
#declare FloorY       = RoseAabbMinY - 1.0;
#declare LookAt       = <-1.787, -2.654, 0.487>;
#declare OrbitRadius  = 8.390;
#declare OrbitHeight  = -1.2;
#declare AngleDeg     = 45.0;
#declare Ang0         = atan2(2.0 - LookAt.x, -7.0 - LookAt.z);
#declare Ang          = Ang0 + clock * 2.0 * pi;

camera {
  location <LookAt.x + OrbitRadius * sin(Ang), OrbitHeight, LookAt.z + OrbitRadius * cos(Ang)>
  look_at LookAt
  angle AngleDeg
}

background { color rgb <0.12, 0.14, 0.18> }

light_source { <6, 8, -4> color rgb 1.6 }
light_source { <-4, 0, -6> color rgb <0.4, 0.45, 0.6> }

plane {
  y, FloorY
  pigment { checker color rgb 0.12 color rgb 0.55 scale 0.8 }
  finish { ambient 0.25 diffuse 0.7 }
}

gaussian_splat {
  "models/Rose.ply"
  samples 2
  sh_degree 3
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
