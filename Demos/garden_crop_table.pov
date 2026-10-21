// mipnerf360 garden crop (table) - Open3D 3DGS example PLY via Assimp
// Model: 3D-Objekte/open3d_3dgs_example/mipnerf360_garden_crop_table.ply (~773k splats)
// rotate 180 about X around AABB center: COLMAP/OpenGL Y-down vs POV Y-up
#version 3.8;
global_settings { assumed_gamma 1.0 }

// AABB: min=(-1.15,0.025,0.47) max=(0.81,0.82,1.60) center=(-0.171,0.423,1.037)
camera {
  location <-0.1710, 1.0, -2.5>
  look_at <-0.1710, 0.4231, 1.0366>
  angle 45
}

light_source { <-4, 6, -3> color rgb 1.3 }
light_source { <5, 2, -5> color rgb <0.35, 0.38, 0.45> }
background { color rgb <0.08, 0.09, 0.10> }

gaussian_splat {
  "models/open3d_3dgs_example/mipnerf360_garden_crop_table.ply"
  sh_degree 3
  samples 2
  alpha_stop 0.999
  opacity_cutoff 0.0039
  // upright: 180 about AABB center
  translate <0.17099, -0.423129, -1.03656>
  rotate <180, 0, 0>
  translate <-0.17099, 0.423129, 1.03656>
}

// 4K: ../unix/povray +W3840 +H2160 Antialias=off -D +FN -Oresults/garden_crop_table_4k.png garden_crop_table.pov
