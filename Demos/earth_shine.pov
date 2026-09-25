// Earth with POV-Ray finish (specular + reflection) instead of MTL map_Ks.
// texture_list pre-registers material "EarthSurface" so mtllib maps are skipped;
// albedo + bump come from POV image_map / bump_map; shine via finish.
// Camera framed from Assimp AABB (same as earth.pov).
#version 3.8;
global_settings { assumed_gamma 1.0 }

camera {
  location <2.2485, 1.6061, -6.4243>
  look_at <0.0000, 0.0000, 0.0000>
  angle 40
}

light_source { <-5.1395, 7.0668, -5.7819> color rgb 1.35 }
light_source { <4, 1, 3> color rgb 0.25 }
background { color rgb <0.02, 0.02, 0.05> }

mesh {
  obj "models/Earth/Earth.obj"
  texture_list {
    "EarthSurface"
    texture {
      uv_mapping
      pigment {
        image_map {
          png "models/Earth/Earth_albedo.png"
          interpolate 2
        }
      }
      normal {
        bump_map {
          png "models/Earth/Earth_bump.png"
          interpolate 2
          bump_size 0.35
        }
      }
      finish {
        ambient 0.15
        diffuse 0.65
        specular 0.55
        roughness 0.04
        reflection { 0.18 }
      }
    }
  }
  uv_mapping
}
