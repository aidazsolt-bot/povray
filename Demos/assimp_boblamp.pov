// Assimp animation demo — BobLamp (MD5 mesh + sibling .md5anim)
// Camera framed from Assimp AABB; textures from Assimp material maps.
#version 3.8;
global_settings { assumed_gamma 1.0 }

camera {
  location <71.8388, 85.1661, -206.8161>
  look_at <-0.3406, 33.6094, -0.5895>
  angle 40
}

light_source { <-165.3219, 260.4587, -186.1935> color rgb 1.3 }
light_source { <90, 60, 40> color rgb 0.35 }
background { color rgb <0.18, 0.2, 0.24> }

assimp { "models/BobLamp/boblamp.md5mesh" }
