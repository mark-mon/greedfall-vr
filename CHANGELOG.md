# Changelog

## Unreleased
- Mouse pointer in menus (VR menu > HUD > "Mouse pointer in menus", off by default, plus "Mouse pointer size"): the
  game's own pointer is drawn on the HUD panel / flat screen whenever the game shows it (inventory, level-up, map,
  title menu). While it is on, the whole monitor maps onto the whole game window (which is the headset's size and
  usually larger than the monitor), so every part of a menu can be reached and clicked.

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
