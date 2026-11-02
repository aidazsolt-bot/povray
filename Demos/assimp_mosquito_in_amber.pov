// Assimp import demo — Khronos MosquitoInAmber.glb
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
  location <0.1060, 0.1371, -0.3116>
  look_at <-0.0010, 0.0013, -0.0071>
  angle 40
}

light_source { <-0.2424, 0.3334, -0.2728> color rgb 1.3 }
light_source { <0.15, 0.06, 0.09> color rgb 0.35 }
background { color rgb <0.08, 0.07, 0.06> }

assimp { "models/MosquitoInAmber/MosquitoInAmber.glb" }
