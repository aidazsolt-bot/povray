// Assimp import demo — MarblesForever.glb (insects in glass/marble spheres)
// Camera framed from Assimp AABB.
#version 3.8;
global_settings { assumed_gamma 1.0 }

camera {
  location <0.8572, 2.5593, -3.9491>
  look_at <-0.1096, 0.4323, -0.0818>
  angle 40
}

light_source { <-2.7071, 3.8673, -3.0939> color rgb 1.35 }
light_source { <2.0, 1.2, 1.5> color rgb 0.4 }
background { color rgb <0.15, 0.18, 0.22> }

assimp { "models/MarblesForever/MarblesForever.glb" }
