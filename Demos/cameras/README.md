# Training-camera validation for Gaussian splats
#
# Rose.ply ships **without** COLMAP / transforms.json. Until you drop a real
# capture next to the PLY:
#
#   1) Put transforms.json (NeRF/3DGS) under Demos/cameras/  (or any path)
#   2) Optionally copy frame 0 PNG to Demos/reference/
#   3) Regenerate:
#        python3 tools/transforms_to_pov_camera.py cameras/YOUR.json \
#          --scene --samples 2 --ply models/Rose.ply -o rose_training_camera.pov
#        python3 tools/transforms_to_pov_camera.py cameras/YOUR.json \
#          -o generated/rose_cam_frame0.inc
#
# Current rose_training_*.pov files use a **synthetic** AABB-framing c2w
# (cameras/rose_aabb_c2w.json) only to exercise direction/right/up + Kerbl.
# Photoreal SH match needs the real training pose + reference image side-by-side.
#
# Render:
#   ../unix/povray +W640 +H640 +A0.3 +AM2 +R2 -D -Oresults/rose_train_kerbl.png rose_training_camera.pov
#   ../unix/povray +W640 +H640 +A0.3 +AM2 +R2 -D -Oresults/rose_train_vol.png rose_training_volume.pov
