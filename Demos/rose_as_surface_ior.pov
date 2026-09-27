// as_surface + rose_ior method: SH colours spatially, finish/IOR from material path.
// Local splat opacity weights transmit/specular so denser regions match optically.
#version 3.8;
global_settings { assumed_gamma 1.0 }

camera {
  location <2.0, -1.5, -6.5>
  look_at <-1.82, -2.85, 0.52>
  angle 45
}

background { color rgb <0.12, 0.14, 0.18> }

light_source { <6, 10, -3> color rgb 1.6 }
light_source { <-4, 1, -6> color rgb <0.4, 0.45, 0.6> }

plane {
  z, 3.5
  pigment { checker color rgb 0.12 color rgb 0.55 scale 0.8 }
  finish { ambient 0.25 diffuse 0.7 }
}

gaussian_splat {
  "models/Rose.ply"
  as_surface
  filter_floaters
  surface_smooth
  sh_degree 3
  texture {
    pigment { color rgb 1 } // overwritten per triangle by splat SH
    finish {
      ambient 0.02
      diffuse 0.35
      specular 0.55
      roughness 0.02
      reflection { 0.12 metallic 0.15 }
      emission rgb 0.1
    }
  }
  interior {
    ior 1.33
  }
}

// ../unix/povray +W800 +H600 -D -Oresults/rose_as_surface_ior.png rose_as_surface_ior.pov
