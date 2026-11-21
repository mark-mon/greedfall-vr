# Changelog

## 1.0-rc1 (2026-11-21)
First public release candidate.
- Native stereo for all 16 render threads, head tracking, HUD panel, VR settings menu (Insert / L3+R3).
- Rendering at the headset's resolution with a live Render scale.
- Head-aimed culling, occlusion re-mapping, cutscene culling and depth-of-field fixes.
- Comfort: no head bob, camera shake, depth of field or dynamic resolution.
- NVIDIA VRS foveated rendering (scene only - HUD stays sharp), optional eye-tracked foveation (OpenXR eye gaze).
- LumaSharpen; live draw distance.
- Fix: the ground could stay attached to the headset for a whole session when a shader's camera was found on a
  shadow-pass draw at load (layouts are now repaired within a second).
