# GreedFall VR

A native stereo VR mod for **GreedFall** (Spiders, 2019, Steam, DirectX 11). Each eye is rendered by the game itself
at your headset's resolution, with full head tracking, a floating HUD, comfort fixes, foveated rendering and an
in-headset settings menu. Everything is one file: `dinput8.dll`. No injector, no extra programs.

**Status: 1.0 release candidate 1.**

## Requirements

- GreedFall (Steam version) on Windows 10/11.
- A PC VR headset with an **OpenXR runtime** (developed and tested with SteamVR's OpenXR runtime).
- A fast CPU. The game is **heavily CPU bound** in VR, even on a Ryzen 7 7800X3D (see *Performance*).
- Optional: an NVIDIA RTX GPU for foveated rendering (VRS); a headset with eye tracking for eye-tracked foveation.

## Install

1. Download `dinput8.dll` from the release.
2. Copy it into the GreedFall folder, next to `GreedFall.exe`
   (e.g. `...\steamapps\common\GreedFall\dinput8.dll`).
3. Start SteamVR (or your OpenXR runtime), then start the game from Steam.

To uninstall, delete `dinput8.dll` (and, if you like, `gfvr_config.txt` and the `GFVR` folder it created).
Your game settings file is never changed: the mod swaps in the VR resolution in memory only.

## Recommended game settings

Set these in GreedFall's own options menu:

| Setting | Recommended | Why |
|---|---|---|
| **Draw distance** | **Medium** or **Low** | The biggest CPU cost by far. *Max* can bring any CPU to its knees in VR (also settable live from the VR menu: *Enhancements > Draw distance*). |
| **SSAO** | **Off** | The game's ambient occlusion produces a faint dark "haze" of nearby objects in VR and costs GPU time. |
| **Anti-aliasing** | **FXAA** | Works per eye and is cheap. Use the mod's **Sharpening (LumaSharpen)** (*Enhancements*, ~0.5) to win back the sharpness FXAA takes. |
| **Field of view** | **Centre (middle) setting** | The headset decides the real field of view; the middle setting keeps the game's culling and LOD sane. |
| **Resolution** | Leave it alone | The mod renders at your headset's resolution regardless; use the mod's *Render scale* instead. |

## The VR menu

Press **Insert** or hold **L3 + R3** on the gamepad. The menu floats in front of you; changes apply live and are saved
to `gfvr_config.txt`.

- **View**: Recenter view (also F8), **Stereo 3D**, **Render scale** (1.00 = the headset's recommended resolution,
  1.20 = 1.2x width and height; applies a second after you stop changing it), headset resolution on/off, world scale,
  eye distance, head tracking / position, field of view.
- **Comfort**: remove head bob, remove camera shake, keep dynamic resolution off.
- **Enhancements**: sharpening, **Foveated rendering (VRS)** 0-3, **Foveation eye tracking**, VRS ring sizes, depth of
  field (off by default), draw distance, culling FOV / margin, TAA jitter, game camera follows head.
- **HUD**: HUD on its own floating panel (default) or in the view, HUD distance / width / height, menu screen size,
  **Mouse pointer in menus** (for keyboard + mouse players) and its size.
- **Debug**: switches used to isolate problems, frame capture, headset image save. You should not need these.
- **Controls**: which pad buttons open the menu.

## Features

- **Native stereo**: the game renders every eye with its own camera (all 16 of the engine's render threads are
  handled), including tessellated terrain, shadows, fog, motion vectors and post effects.
- **Headset resolution**: the game renders each eye at the headset's recommended size x *Render scale* (not a 16:9
  picture stretched over the lens).
- **Head-aimed culling**: the game culls for where you look, so objects don't pop in and out when you turn your head;
  occlusion culling is re-mapped to the headset view; cutscene cameras are handled too.
- **HUD and menus** on a floating panel; flat menus and videos on a virtual screen.
- **Comfort**: no head bob, no camera shake, no depth-of-field blur (cutscenes included), no dynamic resolution.
- **Foveated rendering** (NVIDIA VRS): lower shading rate toward the edges of each eye, only in the 3D scene (the HUD
  stays sharp). +10-20% fps when GPU bound. With an eye-tracking headset that exposes OpenXR eye gaze
  (`XR_EXT_eye_gaze_interaction`), the full-rate area can follow your eyes.
- **LumaSharpen** on the eye images.
- OpenXR in-process on the game's own D3D11 device; all OpenXR frame calls on one thread for smooth pacing.

## Performance

GreedFall's engine is CPU bound in VR: it has to cull and submit both eyes' worth of scene from a wider view, and its
*Draw distance* option multiplies the number of objects. Measured: *Max* draw distance costs ~15-20% more CPU time per
frame than *Medium*, which is the difference between smooth and stuttering in towns. GPU-side, VRS and *Render scale*
let you trade sharpness for frame rate.

## Known issues

- The game lays out its HUD for the tall headset picture, so the compass and bars sit near the top of the HUD panel
  (use *HUD > HUD height / distance* to place it).
- Cutscenes are filmed by the game with telephoto cameras; in VR you see more around the framing than intended.
- Eye-tracked foveation depends on your runtime passing eye gaze to OpenXR; the menu's status line says
  *"VRS: eyes tracked"* when it works.

## Troubleshooting / bug reports

The log is `GFVR\gfvr.log` in the game folder (a new `GFVR\run_<date>` folder holds captures). Please attach the log
to bug reports. *Debug > Capture frame (F11)* saves a trace and the headset images for rendering problems.

## Building from source

The DLL is built with clang-cl + lld-link (cross-compiled from Linux in development):

```
TC=/path/to/toolchain-wrapper bash scripts/build.sh x64     # -> dist64/dinput8.dll
```

`$TC/build.sh <arch> <output> <compiler args> /link <linker args>` must compile against the MSVC CRT and Windows SDK;
`scripts/tc_build_example.sh` is an example using zig's clang and an `xwin`-splatted SDK. Technical notes on how the
game was converted are in [docs/TECHNICAL_NOTES.md](docs/TECHNICAL_NOTES.md); third-party code is listed in
[THIRD_PARTY.md](THIRD_PARTY.md).

## Credits

Built on the UEVR3 D3D9/D3D11 stereo core (see `docs/UEVR3_ARCHITECTURE.md`). LumaSharpen by CeeJay.dk.
