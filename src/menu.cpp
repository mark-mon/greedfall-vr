// FF XIII adapter for the vrkit in-game settings menu (sharedtools\vrkit, D3D9 backend).
// Registers the config.txt settings that make sense to change in the headset, pointing straight at G().s, and wires
// each one to what applies it live. Settings the game only reads at start-up are marked "restart".
#include "menu.h"
#include "globals.h"
#include "d3d9hooks.h"
#include "stereobatch.h"
#include "stereo.h"
#include "camfov.h"
#include "xrbridge/client.h"
#include "vrkit_options.h"
#include "vrkit_menu_d3d9.h"
#include "vrkit_menu.h"
#include "d3d11hooks.h"
#include "xr_inproc.h"
#include "vrkit_input.h"
#include <string>
#include <cstdio>
namespace fvr { void RequestCapture11(); int GfFxCount(); std::string GfFxSelect(int i); }   // d3d11hooks.cpp (menu Debug > Capture frame)

namespace fvr {

namespace {
bool g_ok = false;
bool g_ok11 = false;     // D3D11 games: the D3D11 backend of the same menu
bool g_registered = false;
// choice settings kept as text for the menu (the config stores them as text too). Restart-only settings are copies:
// they are saved, but the running game keeps what it started with.
std::string g_vrMethod, g_msaa, g_fovMode, g_chord, g_hudMode;
float g_renderScale = 1.f;
bool g_desktopVsync = false;

void LogToMod(const char* m) { LOGI("%s", m); }

void ApplyHud() { xrb::ClientSetHud(G().s.hudDistance, G().s.hudWidth, G().s.hudOffsetY, false); }
void ApplyScreen() { xrb::ClientSetMenu(G().s.screenDistance, G().s.screenWidth); }

#ifdef GFVR
// GreedFall VR: only what works in this game (D3D11 backend)
void RegisterGF() {
    using vrkit::Options;
    Options& o = Options::Get();
    if (!o.All().empty()) return;
    Settings& s = G().s;
    g_chord = s.menuChord;
    // ---- View
    o.AddAction("View", "Recenter view", [] { xrb::ClientRequestRecenter(); }, "Make the current head position the centre (also F8).");
    o.AddBool("View", "d3d11_stereo", "Stereo 3D", &s.d3d11Stereo,
              "Off = the game on a flat screen in the headset. Turning it back on needs a game restart if the game started with it off.");
    o.AddFloat("View", "render_scale", "Render scale", &s.renderScale, 0.5f, 2.f, 0.05f, 2,
               "Resolution per eye relative to the headset's recommended size (its 100%): 1.00 = that size, 1.20 = 1.2x as wide "
               "and 1.2x as tall (1.44x the pixels). Applies live about a second after you stop changing it (brief flicker).",
               [] { D3D11RenderScaleChanged(); });
    o.AddBool("View", "d3d11_headset_res", "Render at the headset's resolution", &s.d3d11HeadsetRes,
              "On (default) = each eye at the headset's own size x Render scale. Off = the game's own 16:9 resolution "
              "(Render scale does nothing then). Takes effect at the next start.").needsRestart = true;
    o.AddFloat("View", "world_scale", "World scale", &s.worldScale, 50.f, 200.f, 5.f, 0,
               "Game units per metre (GreedFall uses cm: 100). Higher = the world feels smaller.");
    o.AddFloat("View", "ipd_scale", "Eye distance", &s.ipdScale, 0.f, 2.f, 0.05f, 2,
               "1 = your headset's eye distance. 0 = both eyes at the centre (no 3D depth).");
    o.AddBool("View", "head_tracking", "Head tracking", &s.headTracking, "The view turns with your head (around the game camera).");
    o.AddBool("View", "head_position", "Head position", &s.headPosition, "Also follow head movement (lean in). Off = rotation only.");
    auto& fov = o.AddChoice("View", "fov_mode", "Field of view", &g_fovMode, {"1", "0"},
                            "Headset = fills your view. Game = the game's own narrower view (a window into the world).",
                            [] { G().s.fovMode = atoi(g_fovMode.c_str()); });
    g_fovMode = std::to_string(s.fovMode);
    fov.choiceLabels = {"Headset", "Game"};
    // ---- Comfort
    o.AddBool("Comfort", "no_head_bob", "Remove head bob", &s.gfNoHeadBob,
              "The camera no longer bobs and sways with the character's steps (strongly recommended in VR).");
    o.AddBool("Comfort", "no_camera_shake", "Remove camera shake", &s.gfNoCameraShake, "Hits, explosions and effects no longer shake the camera.");
    o.AddBool("Comfort", "no_dynamic_resolution", "Dynamic resolution off", &s.gfNoDynamicRes,
              "The game keeps a fixed render resolution (dynamic resolution would blur the image in VR).");
    // ---- Enhancements
    o.AddBool("Enhancements", "keep_taa_jitter", "Keep TAA jitter", &s.keepJitter,
              "Keeps the game's anti-aliasing sub-pixel jitter in the headset view (smoother edges). Off if you see shimmering.");
    o.AddBool("Enhancements", "engine_head_v2", "Game camera follows head", &s.engineHead,
              "On (default): the game culls, picks detail and draws shadows for where you look, so nothing pops in or out when you turn your head. "
              "Off: the game only sees its own camera (objects at the edge of your view can vanish).");
    o.AddFloat("Enhancements", "sharpen", "Sharpening (LumaSharpen)", &s.sharpen, 0.0f, 5.0f, 0.05f, 2,
               "Sharpens the headset image (brightness only, no colour shift) - good with the game's FXAA, which blurs a "
               "little. 0 = off, 0.65 = ReShade's default, higher = sharper.");
    o.AddFloat("Enhancements", "sharpen_clamp", "Sharpening limit", &s.sharpenClamp, 0.005f, 0.2f, 0.005f, 3,
               "Largest brightness change sharpening may make per pixel (0.035 default). Higher = stronger edges, more halos.");
    o.AddInt("Enhancements", "vrs", "Foveated rendering (VRS)", &s.vrsStrength, 0, 3, 1,
             "NVIDIA variable rate shading: the edges of each eye are shaded at a lower rate, the centre at full rate. "
             "0 off, 1 gentle (1x2 / 2x2), 2 balanced (2x2), 3 strong (2x2 / 4x4). Saves GPU time, not CPU time.");
    o.AddBool("Enhancements", "vrs_eye_tracking", "Foveation eye tracking", &s.vrsEyeTracking,
              "Off (default) = the full-rate area stays at the centre of each lens. On = it follows your eyes, with a headset "
              "that gives eye tracking to OpenXR (e.g. Pimax Crystal, Quest Pro over Link, Varjo); the two radii below can then "
              "be much smaller (try 0.30 / 0.60). The status line shows 'eyes tracked' when it works.");
    o.AddFloat("Enhancements", "vrs_inner", "VRS full-rate area", &s.vrsInner, 0.2f, 1.2f, 0.05f, 2,
               "Radius of the full-rate centre, as a fraction of half the eye's height (1.0 = top / bottom edge).");
    o.AddFloat("Enhancements", "vrs_outer", "VRS outer ring starts", &s.vrsOuter, 0.3f, 1.6f, 0.05f, 2,
               "Radius where the lowest rate starts (between the two radii: the middle rate).");
    o.AddBool("Enhancements", "game_dof", "Depth of field", &s.gameDof,
              "The game's depth-of-field blur (strong in cutscenes, made for the flat film camera). Off (default) = sharp "
              "everywhere, which suits VR; on = the game's own setting.");
    o.AddInt("Enhancements", "game_lod_distance", "Draw distance (-1 = game's option)", &s.gfLodDist, -1, 3, 1,
             "The game's draw distance, live: 0 low, 1 medium, 2 high, 3 max; -1 = leave the game's own option. The biggest "
             "CPU cost in busy views (max: all distant objects are drawn). Medium or high is recommended in VR.");
    o.AddFloat("Enhancements", "cull_fov", "Culling FOV", &s.cullFov, 1.f, 3.f, 0.1f, 1,
               "Lets the game draw objects outside its own narrow camera view (edges of your headset view, looking around). "
               "Higher = fewer pop-ins at the edges, a bit slower and slightly lower detail far away. Applies when the camera next changes.");
    o.AddFloat("Enhancements", "cull_margin", "Culling margin", &s.cullMargin, 1.0f, 2.0f, 0.05f, 2,
              "With the game camera following your head: how much wider than the headset view the game culls. "
              "Raise it if things still pop in at the edges during fast head turns; lower = a little faster.");

    // ---- HUD & screen
    o.AddBool("HUD", "d3d11_hud_layer", "HUD on its own panel", &s.d3d11HudLayer,
              "On = compass, health and menus on a floating panel. Off = drawn into the 3D view.");
    o.AddFloat("HUD", "hud_distance", "HUD distance (m)", &s.hudDistance, 0.5f, 5.f, 0.1f, 1, "How far away the HUD panel floats.", ApplyHud);
    o.AddFloat("HUD", "hud_width", "HUD width (m)", &s.hudWidth, 0.4f, 5.f, 0.1f, 1, "Width of the HUD panel (and of this menu).", ApplyHud);
    o.AddFloat("HUD", "hud_offset_y", "HUD height (m)", &s.hudOffsetY, -1.f, 1.f, 0.05f, 2, "Move the HUD panel up or down.", ApplyHud);
    o.AddFloat("HUD", "screen_distance", "Screen distance (m)", &s.screenDistance, 1.f, 10.f, 0.1f, 1,
               "Menus, loading screens and movies: distance of the flat screen.", ApplyScreen);
    o.AddFloat("HUD", "screen_width", "Screen width (m)", &s.screenWidth, 1.f, 10.f, 0.1f, 1, "Width of the flat screen.", ApplyScreen);
    o.AddBool("HUD", "mouse_pointer", "Mouse pointer in menus", &s.mousePointer,
              "Off (default). On = the game's mouse pointer is drawn on the menu / HUD panel whenever the game shows it "
              "(inventory, level-up, map, title menu), for keyboard + mouse players. Hidden in normal gameplay.");
    o.AddFloat("HUD", "mouse_pointer_size", "Mouse pointer size", &s.mousePointerSize, 0.5f, 4.f, 0.1f, 1,
               "Size of the mouse pointer on the panel (1.0 = as on a 1080p monitor).");
    // ---- Debug: test switches (all take effect at once - no restart)
    o.AddBool("Debug", "cb_decl_filter", "Patch declared buffers only", &s.cbDeclFilter,
              "On = the headset camera is only written into constant buffers a shader really uses as such (fix for phantom "
              "sails / poles / slabs). Off = old behaviour, for comparison.");
    o.AddBool("Debug", "list_cb_reupload", "Per-list camera upload", &s.listCbReupload,
              "On = every game command list uploads its own headset camera data (fix for geometry / streaks flashing up "
              "for a single frame). Off = old behaviour, for comparison.");
    o.AddBool("Debug", "xr_one_thread", "OpenXR on one thread", &s.xrOneThread,
              "On = all headset calls run on one thread (fix for the game looking like a slideshow while the fps is high). "
              "Off = old behaviour, for comparison.");
    o.AddBool("Debug", "xr_start_pacing", "Frame start pacing", &s.xrStartPacing,
              "On = the headset frame starts right after the game presents (new). Off = old order (everything at Present). "
              "Compare smoothness with the menu closed.");
    o.AddInt("Debug", "gpu_queue_limit", "GPU queue limit (frames)", &s.gpuQueueLimit, 0, 3, 1,
             "How many frames the game may run ahead of the GPU (0 = no limit, the default).");
    o.AddBool("Debug", "xr_fix_late_time", "Fix late display time", &s.xrFixLateTime,
              "SteamVR sometimes hands out frame display times that are already in the past (frames then count as late). "
              "On = such frames get a proper future time (default off; not needed with OpenXR on one thread).");
    o.AddBool("Debug", "desktop_vsync_off", "Desktop vsync off in VR", &s.desktopVsyncOff,
              "On = the desktop window does not wait for the monitor while the headset shows the game.");
    o.AddInt("Debug", "occlusion_mode", "Occlusion culling (0 game/1 fixed/2 off/3 fixed+fast)", &s.gfOcclusion, 0, 3, 1,
             "The game hides objects it thinks are behind others, using a small depth image of the left eye. 0 = as the game "
             "does it (objects next to near ones vanish while turning the head), 1 = that image is re-mapped to the game's camera, "
             "2 = nothing is hidden this way (costs a lot of fps), 3 = like 1, and objects well outside both eyes' view "
             "are hidden too (faster; default).");
    o.AddBool("Debug", "game_taa_off", "Game TAA off", &s.gfTaaOff, "Turns off the game's temporal anti-aliasing (edges get more jagged).");
    o.AddBool("Debug", "prev_vp_copy", "Motion vectors follow head", &s.prevVpCopy,
              "The game's motion vectors read a second copy of the previous camera; on = it gets the eye cameras too. "
              "Off = the old behaviour (faint shadow copies of nearby objects while turning the head).");
    o.AddBool("Debug", "spin_lock", "Spinning lock (CPU)", &s.spinLock,
              "On = the game's render threads wait for the mod's shared lock by spinning briefly (it is held for microseconds) "
              "instead of going to sleep. Off = they yield at once, for comparison.");
    o.AddBool("Debug", "lock_free", "Lock-free buffer tracking (CPU)", &s.lockFree,
              "On = the game's 16 render threads track their buffer updates without waiting for each other. "
              "Off = old behaviour (one shared lock; slow in busy views), for comparison.");
    o.AddBool("Debug", "skip_big_cb", "Skip copies of big buffers (CPU)", &s.skipBigCb,
              "On = the mod no longer copies the game's large per-object data buffers (up to 64 KB each, thousands per frame) "
              "that it never changes anyway. Saves CPU in busy views. Off = old behaviour, for comparison.");
    o.AddBool("Debug", "layout_merge", "Complete camera data across effects", &s.layoutMerge,
              "On = every effect gets all the camera values the mod knows for the game's camera buffer (fix for the haze: "
              "the ambient occlusion pass used the game camera's view). Off = old behaviour (some effects only half converted). "
              "Live A/B switch.");
    o.AddBool("Debug", "prev_frame_pose", "TAA head motion", &s.prevFramePose,
              "Tells the game's anti-aliasing how your head moved since the last frame.");
    o.AddBool("Debug", "cull_follow_head", "Culling follows head", &s.cullFollowHead,
              "Widens the game's culling angle with your head direction (old fix attempt; distorts at large angles).");
    o.AddAction("Debug", "Capture frame (F11)", [] { RequestCapture11(); },
                "Saves a 3-frame draw trace and headset images to GFVR\\run_* (attach them to a bug report).");
    {
        static int fxIdx = 0;   // not saved (no key): starts at "none" every launch
        auto relabel = [] {
            int n = GfFxCount();
            if (fxIdx > n) fxIdx = n;
            std::string d = GfFxSelect(fxIdx);
            for (auto& op : vrkit::Options::Get().All())
                if (op.i == &fxIdx) {
                    char b[64]; snprintf(b, sizeof b, "Hide effect pass (%d found)", n);
                    op.label = b;
                    op.help = "Skips one full-screen effect pass (post processing) at a time in both eyes, to find the one behind "
                              "the haze. Left/right steps through them; 0 = nothing hidden. Now: " + d +
                              ". Some passes black out or freeze the picture - that's expected, just step on. Passes are listed "
                              "as the game first uses them (play a moment first); note the number that removes the haze.";
                }
        };
        o.AddInt("Debug", "", "Hide effect pass", &fxIdx, 0, 120, 1,
                 "Skips one full-screen effect pass (post processing) at a time, to find the one behind the haze. Left/right steps; 0 = none.", relabel);
    }
    o.AddBool("Debug", "eye_mat_cache", "Cache eye cameras per thread (CPU)", &s.eyeMatCache,
              "On = each of the game's render threads keeps its own copy of this frame's eye cameras instead of asking a shared "
              "lock twice per object. Same picture. Off = old behaviour, for comparison.");
    o.AddBool("Debug", "draw_cam_cache", "Reuse camera check between objects (CPU)", &s.drawCamCache,
              "On = when an object uses the same shaders and the same unchanged camera data as the one before it, the mod's camera "
              "search is not repeated. Same picture. Off = old behaviour, for comparison.");
    o.AddBool("Debug", "test_left_only", "Perf test: right eye not drawn", &s.testLeftOnly,
              "Test only: the right eye is not drawn (it shows garbage), to measure how much CPU the second eye costs. Leave off.");
    o.AddBool("Debug", "cine_cull_widen", "Cutscene culling fix", &s.cineCull,
              "Cutscenes film with a narrow camera (5-20 deg). On (default) = the game culls for the whole headset view like "
              "in gameplay; off = old behaviour (heads / floor vanish when you look a little up or down in cutscenes).");
    o.AddBool("Debug", "vrs_per_eye", "VRS per-eye centres", &s.vrsPerEye,
              "Off (default) = one rate image for both eyes (full rate around both eye centres; no extra CPU work). "
              "On = each eye its own image (slightly more saving, one more state change per object).");
    o.AddBool("Debug", "vrs_fullscreen", "VRS on full-screen passes", &s.vrsFullscreen,
              "On = foveated rendering also lowers the shading rate of full-screen passes at eye size (lighting, effects) - "
              "most of the GPU saving. Off = only geometry drawn with depth (closer to GranblueVR's rule).");
    o.AddBool("Debug", "vrs_scene_only", "VRS only in the 3D scene", &s.vrsSceneOnly,
              "On (default) = the HUD, menus and markers are always drawn at full rate (they were unreadable with VRS). "
              "Off = old behaviour (every eye-size target, test only).");
    o.AddBool("Debug", "layout_repair", "Repair camera layouts", &s.layoutRepair,
              "On (default) = a shader that picked up the previous frame's camera at load (ground / objects stuck to your head "
              "until a restart) is switched to the current camera within a second. Off = old behaviour (test only).");
    // ---- Controls
    auto& ch = o.AddChoice("Controls", "menu_chord", "Menu button", &g_chord, {"l3r3", "ltrt", "backstart", "none"},
                           "Pad buttons that open this menu (hold). Insert always works.");
    ch.choiceLabels = {"L3 + R3", "LT + RT", "Back + Start", "Keyboard only"};
    ch.needsRestart = true;
}
#endif
void Register() {
#ifdef GFVR
    RegisterGF();
    return;
#endif
    using vrkit::Options;
    Options& o = Options::Get();
    if (!o.All().empty()) return;
    Settings& s = G().s;
    g_vrMethod = s.vrMethod == VR_AER ? "aer" : "native";
    g_msaa = s.renderMsaa < 0 ? "game" : std::to_string(s.renderMsaa);
    g_fovMode = std::to_string(s.fovMode);
    g_chord = s.menuChord;
    g_renderScale = s.renderScale;
    g_hudMode = s.hudLayer ? "layer" : "eyes";

    // ---- Mode: the four test stages of The Last Remnant VR as one-click presets (they set the options below)
    auto preset = [&s](bool stereo, float ipd, bool hudLayer) {
        s.stereo = stereo; s.ipdScale = ipd; s.hudLayer = hudLayer; g_hudMode = hudLayer ? "layer" : "eyes";
        vrkit::Options::Get().MarkDirty();   // saved to ue3vr_config.txt when the menu closes
        LOGI("settings menu: preset stereo %d, eye distance %.2f, HUD %s", stereo, ipd, hudLayer ? "layer" : "eyes");
    };
    o.AddAction("Mode", "1  Theater (flat screen)", [preset] { preset(false, 1.f, true); },
                "The whole game on a flat virtual screen. No head tracking of the game view.");
    o.AddAction("Mode", "2  Head tracking, mono, HUD in view", [preset] { preset(true, 0.f, false); },
                "3D scene follows your head at the headset FOV; both eyes the same image; HUD drawn into the view.");
    o.AddAction("Mode", "3  Head tracking, mono, HUD panel", [preset] { preset(true, 0.f, true); },
                "As 2, with HUD / minimap / menus on their own panel.");
    o.AddAction("Mode", "4  Full stereo + HUD panel", [preset] { preset(true, 1.f, true); },
                "Each eye rendered with its own camera (the full mod). The choice is saved when the menu closes.");

    // ---- View
    o.AddAction("View", "Recenter view", [] { xrb::ClientRequestRecenter(); }, "Make the current head position the centre (also F8).");
    o.AddFloat("View", "world_scale", "World scale", &s.worldScale, 25.f, 400.f, 5.f, 0,
               "Game units per metre (the game uses cm: 100). Higher = the world feels smaller, eye distance smaller.");
    o.AddFloat("View", "ipd_scale", "Eye distance", &s.ipdScale, 0.f, 2.f, 0.05f, 2,
               "1 = your headset's eye distance. 0 = both eyes at the centre (flat view with head tracking).");
    o.AddBool("View", "head_tracking", "Head tracking", &s.headTracking, "The view turns with your head (around the game camera).");
    o.AddBool("View", "head_position", "Head position", &s.headPosition, "Also follow head movement (lean in). Off = rotation only.");
    auto& fov = o.AddChoice("View", "fov_mode", "Field of view", &g_fovMode, {"1", "0"},
                            "Headset = fills your view. Game = the game's own narrower view (a window into the world).",
                            [] { G().s.fovMode = atoi(g_fovMode.c_str()); });
    fov.choiceLabels = {"Headset", "Game"};
    o.AddFloat("View", "cull_fov", "Culling FOV", &s.cullFov, 1.f, 6.f, 0.25f, 2,
               "Draw objects the game's own camera wouldn't see (the top and bottom of your view, looking around). Costs some frame rate. 1 = off.");

    // ---- GreedFall comfort fixes
    o.AddBool("Comfort", "no_head_bob", "Remove head bob", &s.gfNoHeadBob,
              "The camera no longer bobs and sways with the character's steps (strongly recommended in VR).");
    o.AddBool("Comfort", "no_camera_shake", "Remove camera shake", &s.gfNoCameraShake,
              "Hits, explosions and effects no longer shake the camera.");
    o.AddBool("Comfort", "no_dynamic_resolution", "Dynamic resolution off", &s.gfNoDynamicRes,
              "The game keeps a fixed render resolution (its dynamic resolution blurs the image in VR). Takes effect when the game next checks the setting.");

    // ---- Stereo
    o.AddBool("Stereo", "stereo", "Stereo 3D", &s.stereo, "Off = the game on a flat screen (theater).");

    // ---- Resolution
    o.AddBool("Resolution", "d3d11_headset_res", "Render at the headset's resolution", &s.d3d11HeadsetRes,
              "On = each eye is rendered at the headset's recommended size x Render scale (its own shape, e.g. 3980x3424). "
              "Off = the game's own resolution (16:9). Takes effect at the next start.").needsRestart = true;
    o.AddFloat("Resolution", "render_scale", "Render scale", &g_renderScale, 0.5f, 2.f, 0.05f, 2,
               "Resolution per eye relative to the headset's recommended size (its 100%): 1.00 = that size, 1.20 = 1.2x as wide "
               "and 1.2x as tall (1.44x the pixels). Press 'Apply resolution now' below, or it applies at the next start.").needsRestart = true;
    o.AddAction("Resolution", "Apply resolution now", [&s] {
        s.renderScale = g_renderScale;
        if (g_ok11) D3D11ApplyRenderScale(); else RequestResolutionReset();
    }, "Asks the game to recreate its render targets at the new size (brief black flicker). If nothing changes or the game "
       "misbehaves, restart it instead (the value is saved either way).");

    // ---- HUD & screen
    auto& hm = o.AddChoice("HUD", "hud_mode", "HUD", &g_hudMode, {"layer", "eyes"},
                           "Layer = minimap and menus on their own panel. Eyes = drawn into the 3D view (head-locked).",
                           [] { G().s.hudLayer = g_hudMode != "eyes"; });
    hm.choiceLabels = {"Own panel", "In the view"};
    o.AddFloat("HUD", "hud_alpha_gain", "Minimap opacity", &s.hudAlphaGain, 0.5f, 4.f, 0.1f, 1,
               "Minimap on the HUD panel: opacity = brightness x this.");
    o.AddFloat("HUD", "hud_distance", "HUD distance (m)", &s.hudDistance, 0.5f, 5.f, 0.1f, 1, "How far away the HUD panel floats.", ApplyHud);
    o.AddFloat("HUD", "hud_width", "HUD width (m)", &s.hudWidth, 0.4f, 5.f, 0.1f, 1, "Width of the HUD panel (and of this menu).", ApplyHud);
    o.AddFloat("HUD", "hud_offset_y", "HUD height (m)", &s.hudOffsetY, -1.f, 1.f, 0.05f, 2, "Move the HUD panel up or down.", ApplyHud);
    o.AddFloat("HUD", "screen_distance", "Screen distance (m)", &s.screenDistance, 1.f, 10.f, 0.1f, 1,
               "Menus and movies: distance of the flat screen.", ApplyScreen);
    o.AddFloat("HUD", "screen_width", "Screen width (m)", &s.screenWidth, 1.f, 10.f, 0.1f, 1, "Menus and movies: width of the flat screen.",
               ApplyScreen);

    // ---- Debug: images for bug reports and switches that isolate rendering problems. Files go to
    //      <game>\UE3VR\run_<date>\ (shots\, capture_<n>\); ue3vr.log says which.
    o.AddAction("Debug", "Save headset images", [] { RequestAtlasShot(); },
                "Left eye | right eye | HUD exactly as sent to the headset (PNG + alpha) -> UE3VR\\run_*\\shots.");
    o.AddAction("Debug", "Save game screenshot", [] { RequestScreenshot(); }, "The game's own window image -> UE3VR\\run_*\\shots.");
    o.AddAction("Debug", "Capture this frame", [] { RequestCapture(); },
                "Every draw of one frame with shaders, constants and render-target images -> UE3VR\\run_*\\capture_*. Takes a few seconds (also F11).");
    o.AddAction("Debug", "Search culling FOV again", [] { CamFovScan(); }, "Restart the culling-FOV search (the frame rate drops while it runs).");
    o.AddBool("Debug", "debug_shadow_patch", "Per-eye shadows", &s.dbgShadowPatch,
              "On = shadows projected for each eye. Off = both eyes use the game's shadow projection (test for black / wrong shadows).");
    o.AddBool("Debug", "debug_hide_shadows", "Hide dynamic shadows", &s.dbgHideShadows,
              "Skips the game's dynamic (modulated) shadow drawing. If black patches disappear, the shadows are the cause.");
    o.AddBool("Debug", "debug_fog_patch", "Per-eye fog", &s.dbgFogPatch, "On = height fog computed for each eye. Off = the game's values.");

    // ---- Tweaks: fixes found in one game, named by what they do so any game can use them. A game's profile may turn
    //      some on by default (its notes say why); ue3vr_config.txt / this page override that.
    o.AddBool("Tweaks", "keep_texture_copies", "Keep texture copies", &s.keepTextureCopies,
              "Keep the CPU copy of every game texture (like D3D's managed pool). Off = free it after upload: saves address "
              "space but games that update textures later show black / missing textures or letters.");
    o.AddInt("Tweaks", "texture_copy_budget_mb", "Texture copy budget (MB)", &s.textureCopyBudgetMb, 256, 2048, 128,
             "Above this (or when the game is short of address space) copies unused for a while are dropped.");
    o.AddBool("Tweaks", "ui_in_scene_flat", "Menus in the 3D scene: flat", &s.uiInSceneFlat,
              "Frames where a menu is drawn into the 3D scene go to the flat screen instead of being stuck to your view.");
    o.AddBool("Tweaks", "shared_target_head", "Reflections follow the head", &s.sharedTargetHead,
              "Scene views drawn into shared square textures (floor / water reflections) use the head-tracked camera. "
              "Off = the game's camera (reflections can ghost and move with your head).");
    o.AddInt("Tweaks", "stereo_min_draws", "Min. 3D draws for stereo", &s.stereoMinDraws, 1, 200, 1,
             "Frames with fewer 3D draws stay on the flat screen (title screens, menus).");
    o.AddFloat("Tweaks", "stereo_min_fov", "Min. camera FOV for stereo", &s.stereoMinFov, 0.f, 90.f, 5.f, 0,
               "Cameras narrower than this (degrees) are not treated as 3D scenes (title backdrops). 0 = no limit.");
    o.AddBool("Tweaks", "composite_any", "Any scene copy makes eye images", &s.compositeAnyTwin,
              "Eye images from any draw into the screen that reads the 3D scene (later UE3 post-processing). Off = only UE3's gamma pass.");
    g_desktopVsync = !s.desktopVsyncOff;
    o.AddBool("Tweaks", "desktop_vsync", "Desktop vsync", &g_desktopVsync,
              "On = the game keeps its vsync on the desktop window (caps its frame rate to the monitor). Off = the headset paces VR.").needsRestart = true;

    // ---- Diagnostics
    o.AddBool("Diagnostics", "perf_diag", "Performance log", &s.perfDiag, "GPU / CPU timings in GFVR\\gfvr.log every 10 s.");
    auto& ch = o.AddChoice("Diagnostics", "menu_chord", "Menu button", &g_chord, {"l3r3", "ltrt", "backstart", "none"},
                           "Pad buttons that open this menu (hold). Insert always works.");
    ch.choiceLabels = {"L3 + R3", "LT + RT", "Back + Start", "Keyboard only"};
    ch.needsRestart = true;
}

std::string StatusLine() {
    static LONG lastFrame = 0;
    static DWORD lastTick = 0;
    static char text[192] = "";
    DWORD now = GetTickCount();
    if (now - lastTick >= 500) {
        LONG f = G().frame;
        if (lastTick) {
            bool st = false; unsigned n = 0;
            if (g_ok11) D3D11LastFrame(&st, &n);
            else StereoLastFrame(&st, &n);
            char cull[96];
            CamFovStatus(cull, sizeof(cull));
#ifdef GFVR
            const char* et = "";
            if (G().s.vrsStrength > 0 && G().s.vrsEyeTracking) {
                int e = xrb::XrEyeTrackingStatus();
                et = e == 2 ? " | VRS: eyes tracked" : e == 1 ? " | VRS: eyes not found" : e == -1 ? " | VRS: no eye tracking (runtime)" : e == -2 ? " | VRS: no eye tracking (headset)" : "";
            }
            sprintf_s(text, "%.0f fps | %s, %u 3D draws%s", (f - lastFrame) * 1000.0 / (now - lastTick), st ? "stereo" : "flat", n, et);
#else
            sprintf_s(text, "%.0f fps | %s, %u 3D draws | culling: %s", (f - lastFrame) * 1000.0 / (now - lastTick), st ? "stereo" : "flat", n, cull);
#endif
        }
        lastFrame = f;
        lastTick = now;
    }
    return text;
}
} // namespace

vrkit::menu::Settings MenuSettings() {
    if (!g_registered) { vrkit::SetLog(LogToMod); Register(); g_registered = true; }
    vrkit::menu::Settings m;
#ifdef GFVR
    m.title = "GreedFall VR " GFVR_VERSION;
#else
    m.title = std::string("UEVR3 - ") + G().gameName;
#endif
    m.configPath = G().dllDir + L"gfvr_config.txt";
    m.statusLine = StatusLine;
    const Settings& c = G().s;
    m.input.chordHoldMs = c.menuHoldMs;
    m.input.chordButtons = c.menuChord == "l3r3" ? (0x0040 | 0x0080)        // XINPUT_GAMEPAD_LEFT_THUMB | RIGHT_THUMB
                         : c.menuChord == "backstart" ? (0x0020 | 0x0010)  // XINPUT_GAMEPAD_BACK | START (View + Menu)
                         : 0;
    m.input.chordTriggers = c.menuChord == "ltrt";
    m.input.keyboardToggleVk = c.menuKey;
    return m;
}
void LogMenuReady(const char* api) {
    const Settings& c = G().s;
    LOGI("settings menu ready (%s): hold %s %d ms (or Back+Start) or press key 0x%X", api,
         c.menuChord == "l3r3" ? "L3+R3" : c.menuChord == "ltrt" ? "LT+RT" : c.menuChord == "backstart" ? "Back+Start" : "(no pad)", c.menuHoldMs, c.menuKey);
}
// "the menu doesn't open": what the menu's input sees
void MenuInputDiag() {
    static int lastPad = -2;
    int pad = vrkit::input::PadUser();
    if (pad != lastPad) {
        if (pad >= 0) LOGI("settings menu: XInput pad found (user %d)", pad);
        else if (lastPad >= 0 || lastPad == -2) LOGI("settings menu: no XInput pad found (the keyboard key still works)");
        lastPad = pad;
    }
    static bool keyWas = false;
    bool keyNow = G().s.menuKey && (GetAsyncKeyState(G().s.menuKey) & 0x8000) != 0;
    if (keyNow && !keyWas) LOGI("settings menu: key 0x%X pressed", G().s.menuKey);
    keyWas = keyNow;
}

void MenuInit(IDirect3DDevice9* dev) {
    if (g_ok || g_ok11 || !dev) return;
    vrkit::menu9::Settings9 s;
    s.menu = MenuSettings();
    s.menu.fontOversample = 2.f;   // drawn up to ~1.8x larger into the HUD region
    s.menu.backgroundAlpha = 1.f;  // the HUD (minimap etc.) is behind it
    s.restore = BsRestoreDevice;
    InternalScope in;
    g_ok = vrkit::menu9::Init(dev, s);
    if (!g_ok) LOGE("settings menu: init failed (menu unavailable)");
    else LogMenuReady("D3D9");
}

bool MenuUpdate() {
    if (!g_ok) return false;
    static bool wasOpen = false;
    MenuInputDiag();
    bool open = vrkit::menu9::Update();
    if (open != wasOpen) { LOGI("settings menu %s", open ? "opened" : "closed"); wasOpen = open; }
    return open;
}

// ---- D3D11 (the menu is drawn into its own texture; the D3D11 backend copies it into the headset image)
bool MenuInit11(ID3D11Device* dev, ID3D11DeviceContext* imm, UINT screenW, UINT screenH) {
    if (g_ok11) return true;
    if (g_ok || !dev || !imm) return false;
    vrkit::menu::Settings m = MenuSettings();
    // a 4:3 panel of at most 80 % x 85 % of the game image (it is copied into the middle of that image)
    unsigned w = 1280, h = 960;
    if (w > screenW * 8 / 10) { w = screenW * 8 / 10; h = w * 3 / 4; }
    if (h > screenH * 85 / 100) { h = screenH * 85 / 100; w = h * 4 / 3; }
    m.texW = w; m.texH = h;
    m.fontPx = 34.f * w / 1280.f;
    m.backgroundAlpha = 1.f;
    g_ok11 = vrkit::menu::Init(dev, imm, m);
    if (!g_ok11) LOGE("settings menu: D3D11 init failed (menu unavailable)");
    else LogMenuReady("D3D11");
    return g_ok11;
}
bool MenuUpdate11() {
    if (!g_ok11) return false;
    static bool wasOpen = false;
    MenuInputDiag();
    bool open = vrkit::menu::Update();
    if (open != wasOpen) { LOGI("settings menu %s", open ? "opened" : "closed"); wasOpen = open; }
    return open;
}
ID3D11Texture2D* MenuTexture11() { return g_ok11 ? vrkit::menu::Texture() : nullptr; }
void MenuShutdown11() {
    if (!g_ok11) return;
    vrkit::menu::Shutdown();
    g_ok11 = false;
}

void MenuRender(IDirect3DDevice9* dev, IDirect3DSurface9* surface, const RECT& rect) {
    if (!g_ok || !vrkit::menu9::IsOpen()) return;
    InternalScope in;
    // a 4:3 panel over the middle ~80 % of the HUD region
    LONG w = rect.right - rect.left, h = rect.bottom - rect.top;
    RECT r = {rect.left + w / 10, rect.top + h / 10, rect.right - w / 10, rect.bottom - h / 10};
    vrkit::menu9::Render(dev, surface, r);
}

bool MenuBlocksGameInput() { return (g_ok || g_ok11) && vrkit::input::GameInputBlocked(); }
void MenuOnLostDevice() {
    if (!g_ok) return;
    InternalScope in;   // our device hooks stay out of the Ex check
    vrkit::menu9::OnLostDevice();
}

} // namespace fvr
