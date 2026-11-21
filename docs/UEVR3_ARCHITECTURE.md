# UEVR3 - architecture plan: a UEVR-like tool for Unreal Engine 3 (2026-11-02)

Based on a read of UEVR (github.com/praydog/UEVR, cloned 2026-11-02, commit 4ee5c6b) and uevr-frontend.

## What we may reuse (licences)
| UEVR part | Licence | Use in UEVR3 |
|---|---|---|
| core (src/, lua-api/, vr-plugin-nullifier/) | All rights reserved | design only, no code |
| UESDK (reflection SDK submodule) | private repo | not available |
| include/ (API.h, API.hpp, Plugin.hpp) | MIT | plugin API shape; implement a compatible subset |
| examples/ | MIT | yes |
| uevr-frontend (C# WPF injector) | MIT | fork with attribution |
| sol2, Lua, ImGui, spdlog, glm | MIT | yes |
Epic engine source (user's licence): reference only, never shipped.

## How UEVR works (summary of the survey)
- **Frontend**: lists processes (x64 only, d3d11/d3d12 loaded), LoadLibraryW injection via CreateRemoteThread of
  three DLLs from its own folder (optional VR-plugin nullifier, the OpenXR/OpenVR loader, UEVRBackend.dll).
  Settings are files only: `%APPDATA%\UnrealVRMod\<exe>\config.txt` (key=value), plus plugins/, scripts/,
  uobjecthook/*.json. Live link: named shared memory "UnrealVRMod" (pid, thread ids) + PostThreadMessage commands
  (ReloadConfig, Quit).
- **Backend**: DllMain -> thread -> Framework: logger, ImGui, plugin early-load, command thread, D3D hook monitor
  (dummy device/swapchain vtable hooks on Present, re-hooked if lost), WndProc subclass, XInput/DInput hooks,
  crash dumps. Mods (FrameworkConfig, VR, UObjectHook, PluginLoader, LuaLoader) with lifecycle callbacks
  (on_initialize, on_frame, on_draw_ui, on_config_load/save, engine-tick hooks...) and self-serializing values.
- **Reflection (UObjectHook)**: GUObjectArray + FName via the private SDK (string-reference scans + layout probes);
  tracks objects by hooking AddObject/destructor; explorer UI: common roots (pawn, controller, camera manager,
  world), objects by class, properties (editable), functions (call via ProcessEvent), components with "attach to
  left/right controller / HMD", saved per game as state paths re-resolved every tick.
- **Rendering**: UE4/5's own stereo device (FFakeStereoRendering vtable replaced) - UE3 has nothing like it. UI
  captured from Slate into its own texture and shown as a quad/cylinder layer.
- **Plugins/Lua**: C API tables (API.h, versioned, append-only), plugins = DLLs exporting uevr_plugin_initialize;
  Lua = sol2 binding over the same C API.

## UEVR3 design
1. **Launcher/frontend** (fork of uevr-frontend, or a small native app):
   - lists Steam UE3 games (our library scanner) and running processes, **32- and 64-bit**, d3d9 or d3d11;
   - **launch mode (main)**: starts the game suspended (CreateProcess CREATE_SUSPENDED, SteamAppId in the
     environment as in our smoke tests), injects, resumes. Needed because UEVR3 must see device creation
     (D3D9 -> D3D9Ex for shared textures, managed-texture emulation, render-target scaling, right-eye twins);
   - attach mode (running game): object explorer / recon only (no VR output for D3D9 - the device can't become Ex);
   - injection with a matching-bitness helper: `x86\UEVR3Inject.exe` / `x64\...`, backends in `x86\` and `x64\`;
   - profiles in `%APPDATA%\UEVR3\<exe>\` (config.txt as now, plugins, scripts, objects/*.json), import/export zip;
   - the proxy-DLL install (copy d3d9.dll into the game folder) stays as the alternative for games that resist
     injection or need the earliest possible start.
2. **Backend** (today's UEVR3 core): restructure into mods with lifecycle callbacks (Rendering D3D9, Rendering
   D3D11, Culling, HUD, Objects, Plugins, Lua) and self-describing values (the vrkit option list already is that).
   Shared memory "UEVR3_<pid>" + command channel for the frontend (reload config, status, quit).
3. **UE3 object system** (our own code, per-game offsets found automatically):
   - find GNames (FNameEntry array: entry 0 = "None", then "ByteProperty", "IntProperty"... in every UE3 build) and
     GObjObjects (TArray<UObject*>) by scanning the data sections for arrays whose entries point back to valid
     names/objects; cross-check with the native function table we already extract (GNatives);
   - probe the UObject layout (Index, Outer, Name, Class offsets) on known objects ("Class Core.Object",
     "Class Core.Class", "Package Core"); UStruct Children/SuperField, UProperty Offset/ArrayDim/ElementSize/Next,
     UFunction Func/flags the same way;
   - ProcessEvent = vtable slot found by matching the native it dispatches to; x86 thiscall / x64;
   - explorer UI (desktop window + in headset): roots (Engine, GameViewportClient, PlayerController, Pawn, Camera,
     WorldInfo, HUD), search by class/name, property view/edit, call parameterless functions, save property states;
   - camera control: set FOVAngle / culling directly instead of the breakpoint search (which some games kill),
     bShowHUD, first-person toggles where games have them (The Last Remnant: CameraUseFpsViewChange).
4. **Plugin API + Lua**: a UEVR API.h-compatible subset (MIT), adapted to UE3 types (UProperty instead of FField/
   FProperty); our own sol2 binding over it, so UEVR plugins/scripts port with small changes.
5. **Rendering** stays ours (per-draw constant patching, D3D9 by name, D3D11 by value - see D3D11_PLAN.md).

## Order of work
1. Object-system finder + read-only explorer (x86, The Last Remnant / Dishonored as test games) - logs and an
   ImGui window, no game changes.
2. Launcher (launch suspended + inject, x86 and x64 helpers), profiles in %APPDATA%.
3. D3D11 stages (D3D11_PLAN.md) and the x64 build.
4. Explorer actions (edit/call/save), camera/FOV via objects, then plugin API + Lua.
