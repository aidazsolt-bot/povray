// Assimp animation demo — BusterDrone (glTF clip 'CINEMA_4D_Basis')
// Camera framed from Assimp AABB.
#version 3.8;
global_settings { assumed_gamma 1.0 }

camera {
  location <318.1806, 178.9181, -909.0903>
  look_at <-0.0010, -48.3545, 0.0000>
  angle 40
}

light_source { <-727.2733, 951.6449, -818.1813> color rgb 1.2 }
light_source { <400, 100, 200> color rgb 0.35 }
background { color rgb <0.12, 0.16, 0.22> }

assimp { "models/BusterDrone/scene.gltf" }
