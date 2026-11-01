// Assimp import demo — Al the gangster (al.obj + materials via Assimp/MTL).
// Native mesh { obj } importer is left disabled (upstream experimental default-off);
// Assimp is the sole model import path in this fork.
// Camera framed from Assimp AABB of the same model.
#version 3.8;
global_settings { assumed_gamma 1.0 }

camera {
  location <5.3953, 3.5056, -15.4150>
  look_at <0.0000, -0.3481, 0.0000>
  angle 40
}

light_source { <-12.3320, 16.6084, -13.8735> color rgb 1.3 }
light_source { <8, 4, 6> color rgb 0.35 }
background { color rgb <0.15, 0.2, 0.3> }

assimp { "models/al.obj" }
