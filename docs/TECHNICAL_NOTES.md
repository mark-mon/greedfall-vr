# GreedFall VR - technical notes

How GreedFall was converted, for anyone porting a similar D3D11 engine. Addresses are offsets in the Steam build's
`Engine.dll` / `Game.dll` (both unprotected x64; `GreedFall.exe` is a SteamStub launcher that imports `Engine.dll`).

## Engine and entry point

- Spiders' Silk Engine on Sony PSSG, Direct3D 11. Proxy DLL: **dinput8.dll** (statically imported via Engine.dll, so it
  loads before the engine starts).
- The scene is recorded on **16 deferred contexts** (~3000-3800 draws per frame), executed as ~100 command lists on the
  immediate context. The game presents from whichever job thread is free.

## D3D11 lessons (generic)

- **Every deferred context has its own vtable on the heap.** Each is hooked at `CreateDeferredContext`; calls go to that
  vtable's originals. Some deferred slots point to the same driver function as an inline-hooked immediate-context
  function - never take such a slot as the "original" (infinite recursion).
- **Per-context state**: bindings, constant-buffer shadows and per-eye buffer copies live per context; command lists
  must not share per-eye uploads (the game executes lists out of recording order).
- **Thread safety**: every piece of shared stereo state touched per draw (eye matrices, caches) must be locked or
  per-thread. Test with a *moving* simulated head - a still head hides races.
- **Tessellation**: GreedFall's terrain is projected in the domain shader. Patch the camera for HS/DS/GS as well
  (bind the per-eye copy wherever another stage has the same buffer as the patched VS slot).
- **Only patch constant buffers a shader declares**, and never dynamically indexed ones (instance / bone arrays): the
  DXBC `dcl_constantbuffer` opcode (0x59, bit 11 = dynamic index) says which. A chance "camera" match in a 64 KB
  instance array threw objects across the world.
- **One camera struct, many passes**: GreedFall's cb0 (1616 bytes) is bound to every pass. Learn its layout once per
  buffer (union of everything any shader found) rather than per shader.
- **Previous-frame matrices**: cb0 also carries the previous frame's View / Projection / VP (rows 55-66, a second VP
  copy at 78-81) for TAA and motion vectors; they are patched with the previous frame's eye transforms. A shader first
  seen on a draw whose current VP was not a perspective camera could take the previous-frame VP as "the" camera and
  stay head-locked for the session; the mod repairs such layouts (*Debug > Repair camera layouts*).
- **OpenXR from a game that presents on random threads**: keep every xrWaitFrame / xrBeginFrame / xrEndFrame on ONE
  thread of your own. SteamVR's runtime cannot pace frame calls that hop threads (it looked like 5 fps at 60+ fps).
- **NVAPI VRS from several recording threads at once crashes** inside the driver: serialize all NVAPI calls.

## cb0 layout (row-vector matrices, right-handed, reversed infinite Z, near 20, 1 unit = 1 cm)

| float4 rows | content |
|---|---|
| 0-3 | View |
| 4-7 | Projection (rows 20/21 = TAA jitter) |
| 8-11 | ViewProjection |
| 44-47 | inverse View (47 = camera position) |
| 48-51 | inverse ViewProjection |
| 53 / 54 | camera position / forward |
| 55-58 / 59-62 / 63-66 | previous View / Projection / VP |
| 78-81 | previous VP (second copy) |

## Camera, culling, occlusion

- `PCamera::setFOV` (0x568530) / `PCameraNode::setFOV` (0x568550): FOV in degrees. Render camera call sites
  Engine+0x34DA98 (gameplay), +0x7126BD (title / menus).
- The engine renders and culls from a per-frame **snapshot** of the camera node (copy at 0x567270): the mod turns that
  snapshot with the head and widens its FOV a little (headset FOV x margin). Large FOV widening instead breaks the
  engine's "too small to draw" / LOD checks (pop-in straight ahead).
- **Occlusion culling** reads back a small depth pyramid of the (left eye) depth to the CPU; the mod re-maps it from the
  eye projection to the game camera's projection, else objects next to near ones vanish while turning.
- Cutscene cameras are 5-20 deg telephoto cameras; culling is widened for them too.
- Camera node: +0xF0 world, +0x160 view, +0x1A0 view*proj, +0x1E0 PCamera (+0x1F0 FOV, +0x200 projection).
  Engine.dll is LTCG-built: hooks inside it must preserve volatile registers callers assume untouched.

## Game settings accessors (dreGraphicOptions, getter(out*) / setter(unused, in*))

| option | get | set |
|---|---|---|
| TAA | 0x34E860 | 0x34E830 |
| FXAA | 0x34E810 | 0x34E7E0 |
| SSAO quality (0 off, 1 low, 2 high) | 0x34E6D0 | 0x34E6A0 |
| Draw distance (LodDistanceQuality 0-3, live) | 0x34E960 | 0x34E930 |

## Render resolution

Window, swap chain and all scene targets are sized from `Documents\GreedFall\graphicconfig.txt` RESX / RESY (and the
desktop in windowed-fullscreen mode), not from the swap chain. The mod hooks the config reader (Engine 0x563320; struct
+8 RESX, +0xC RESY, +0x14 windowed) to substitute the headset size and windowed mode, and the writer (0x563890) to
write the user's values back. A live change resizes the game window; the game answers with ResizeBuffers and recreates
its targets.

## Post effects

- Depth of field composite PS `EEDD5866`: blends toward the blurred image by CoC x cb1[10].y; the mod zeroes that
  value per draw (RENDER\EnableDOF does not reach cutscene DoF).
- Half-res temporal SSAO reads the previous depth / AO history and motion vectors; with SSAO on it showed a faint dark
  copy of nearby objects in VR - SSAO off is recommended.

## Comfort

- Head bob: Game.dll 0x4339F0 camera update (bob offset +0x7F0, roll +0x8DC, pitch +0x9BC) zeroed around the call.
- Camera shake: `ACTIVATION\DisableAllCameraShake` byte kept at 1.
- Dynamic resolution: `RENDER\DynamicRes` getter (Engine 0x376E90) forced false.
