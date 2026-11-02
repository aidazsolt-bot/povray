// Assimp import demo — Khronos DragonDispersion.glb
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
  location <6.5020, 9.0490, -16.6290>
  look_at <0.1240, 0.9541, 1.5232>
  angle 40
}

light_source { <-14.1040, 20.5780, -14.6670> color rgb 1.3 }
light_source { <5.0, 3.0, 4.0> color rgb 0.35 }
background { color rgb <0.06, 0.07, 0.10> }

assimp { "models/DragonDispersion/DragonDispersion.glb" }
