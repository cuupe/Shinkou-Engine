# Generated media fixture

`ufbx-cube-7400-binary.fbx` is the `blender_272_cube_7400_binary.fbx` fixture
from [ufbx v0.20.0](https://github.com/ufbx/ufbx/blob/a63ff0a47485328880b3300e7bcdf01413343a45/data/blender_272_cube_7400_binary.fbx).
Its upstream MIT/public-domain license is preserved in `ufbx-LICENSE`.
It complements the authored ASCII FBX fixture with a real binary FBX cube.

`preview-h264.mp4` is an engine-owned generated 64×48, 4 fps, two-second H.264
test pattern with a two-second GOP, no audio. It exercises real YUV conversion,
keyframe preroll, EOF seeking and image revision changes. It contains no
downloaded media. Regenerate with:

```text
ffmpeg -f lavfi -i testsrc2=size=64x48:rate=4:duration=2 -c:v libx264 -g 8 -pix_fmt yuv420p preview-h264.mp4
```
