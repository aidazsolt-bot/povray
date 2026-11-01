// Assimp import demo — Khronos DamagedHelmet.glb
// Same framing as assimp_damaged_helmet.pov with camera location.z flipped (* -1).
#version 3.8;
global_settings { assumed_gamma 1.0 }

camera {
  location <2.1131, 2.7200, 6.2317>
  look_at <-0.0025, 0.0000, -0.1872>
  angle 40
}

light_source { <-4.8356, 6.6489, -5.4400> color rgb 1.3 }
light_source { <3, 1, 2> color rgb 0.3 }
background { color rgb <0.08, 0.09, 0.12> }

assimp { "models/DamagedHelmet/DamagedHelmet.glb" }
