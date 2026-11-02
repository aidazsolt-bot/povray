// Assimp import demo — Khronos DamagedHelmet.glb
// Camera framed from Assimp AABB. Radiosity + finish emission for glow.
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
  location <2.1131, 2.7200, -6.2317>
  look_at <-0.0025, 0.0000, -0.1872>
  angle 40
}

light_source { <-4.8356, 6.6489, -5.4400> color rgb 1.3 }
light_source { <3, 1, 2> color rgb 0.35 }
background { color rgb <0.08, 0.09, 0.12> }

assimp { "models/DamagedHelmet/DamagedHelmet.glb" }
