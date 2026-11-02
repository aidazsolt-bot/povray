// Assimp import demo — Khronos FlightHelmet.gltf
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
  location <0.7370, 1.3318, -2.1497>
  look_at <-0.0204, 0.3580, 0.0142>
  angle 40
}

light_source { <-1.7311, 2.3803, -1.9475> color rgb 1.25 }
light_source { <1.2, 0.8, 1.0> color rgb 0.35 }
background { color rgb <0.55, 0.62, 0.72> }

assimp { "models/FlightHelmet/FlightHelmet.gltf" }
