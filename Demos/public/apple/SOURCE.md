# Public validation dataset: DX.GL Apple (CC0)

Source: https://huggingface.co/datasets/dxgl/multiview-datasets  
Object: Apple (Polyhaven food_apple_01, CC0) — https://dx.gl/datasets/vCHDLxjWG65d

## Contents

| Path | What |
|------|------|
| `splats/apple.ply` | Pretrained splatfacto 3DGS (SH deg 3) from https://dx.gl/splat/apple.ply |
| `transforms.json` | Nerfstudio transforms (196 views, 1024², FOV 45°) |
| `images/frame_00000.png` (+50, +90, +100) | Training RGB frames |
| `../../cameras/apple_transforms.json` | Copy of transforms for the converter |
| `../../models/apple.ply` | Symlink → `splats/apple.ply` |
| `../../reference/apple_frame_*.png` | Symlinks → `images/` |

## Coordinate note

Raw `transforms.json` is in the capture frame (cameras ~4–5 units from origin).  
The published splatfacto PLY is in a nerfstudio-normalized frame (~5× smaller).  
Use `--position-scale 0.2` when generating POV cameras so training views match the PLY.

```bash
python3 tools/transforms_to_pov_camera.py cameras/apple_transforms.json \
  --scene --samples 2 --position-scale 0.2 --ply public/apple/splats/apple.ply \
  -o apple_training_camera.pov

../unix/povray +W512 +H512 -D -Oresults/apple_train_kerbl.png apple_training_camera.pov
```

Side-by-side: `reference/apple_frame_00000.png` vs `results/apple_train_kerbl.png`
