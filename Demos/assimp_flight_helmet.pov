// Assimp import demo — Khronos FlightHelmet.gltf
// Camera framed from Assimp AABB.
#version 3.8;
global_settings { assumed_gamma 1.0 }

camera {
  location <0.7370, 1.3318, -2.1497>
  look_at <-0.0204, 0.3580, 0.0142>
  angle 40
}

light_source { <-1.7311, 2.3803, -1.9475> color rgb 1.25 }
light_source { <1.2, 0.8, 1.0> color rgb 0.35 }
background { color rgb <0.55, 0.62, 0.72> }

assimp { "models/FlightHelmet/FlightHelmet.gltf" }
