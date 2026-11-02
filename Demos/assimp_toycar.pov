// Assimp import demo — Khronos ToyCar.glb
// Camera framed from Assimp AABB. Radiosity + PBR/emissive finish from importer.
#version 3.8;
global_settings {
  assumed_gamma 1.0
  ambient_light rgb 0.15
  radiosity {
    pretrace_start 0.08
    pretrace_end 0.02
    count 80
    nearest_count 8
    error_bound 0.4
    recursion_limit 2
    low_error_factor 0.7
    gray_threshold 0.0
    minimum_reuse 0.015
    brightness 1.0
  }
}

camera {
  location <0.0720, 0.0875, -0.2002>
  look_at <0.0017, -0.0018, 0.0000>
  angle 40
}

light_source { <-0.1647, 0.2265, -0.1748> color rgb 1.3 }
light_source { <0.10, 0.04, 0.06> color rgb 0.35 }
background { color rgb <0.10, 0.12, 0.15> }

assimp { "models/ToyCar/ToyCar.glb" }
