// FF13 VR - loaded by ffxiiiimg.exe as d3d9.dll (proxy to the system d3d9.dll).
//
// Milestone 1: D3D9 -> D3D9Ex upgrade, config.txt, logging, frame capture, screenshots,
// the flat game image on a virtual screen in the headset (XrBridge), and scripted input for unattended tests.
#include <cmath>
#include "globals.h"
#include "d3d9hooks.h"
#include "input.h"
#include "hookutil.h"
#include "profiles.h"
#include "d3d11hooks.h"
#include "greedfall.h"
#include "crashwatch.h"
#include "xrbridge/client.h"
#include <d3d9.h>
#include "MinHook.h"
#include <csignal>
#include <exception>

namespace fvr {
Globals& G() { static Globals g; return g; }
float SecondsSinceStart() { return (GetTickCount() - G().startTick) / 1000.0f; }
}
using namespace fvr;

// ------------------------------------------------------------------ default config.txt
static const char kDefaultConfig[] =
"# GreedFall VR configuration ({GAME}) - lives next to the game exe. Delete it to get the defaults back.\n"
"# Lines starting with # are comments. Missing settings use their defaults. Restart the game after editing.\n"
"\n"
"# ---- General ----\n"
"# 1 = VR output (starts XrBridgeHost.exe, which talks to SteamVR), 0 = mod loads but only does diagnostics\n"
"vr_enabled = 1\n"
"# 0=error 1=warn 2=info 3=debug 4=trace   (log: GFVR\\gfvr.log)\n"
"log_level = 2\n"
"\n"
"# ---- Display ----\n"
"# 1 = always run the game in a window (recommended)\n"
"force_windowed = 1\n"
"# 1 = keep the game's vsync on the desktop window (caps the game to the monitor); 0 = off (the headset paces VR)\n"
"desktop_vsync = 0\n"
"# 1 = run the game on a D3D9Ex device (required for VR output)\n"
"upgrade_to_ex = 1\n"
"# 3D resolution per eye = the headset's recommended resolution x render_scale (1.00 = 100%, 0.75 = 75%, 1.25 = 125%).\n"
"# 0 = the game's own resolution (its window size). render_width / render_height (both > 0) set an exact size instead.\n"
"render_scale = 1.00\n"
"render_width = 0\n"
"render_height = 0\n"
"\n"
"# ---- Virtual screen (menus, title, movies; and everything when stereo = 0) ----\n"
"# distance and width in meters\n"
"screen_distance = 2.5\n"
"screen_width = 3.2\n"
"# Key that recenters the view (virtual-key code; 0x77 = F8)\n"
"recenter_key = 0x77\n"
"\n"
"# ---- Stereo 3D / head tracking (field, battles, real-time scenes) ----\n"
"# 1 = render the 3D scene per eye with head tracking, 0 = flat screen only (theater mode)\n"
"stereo = 1\n"
"# 1 = headset FOV (fills your view), 0 = the game's FOV (a smaller window into the world)\n"
"fov_mode = 1\n"
"# eye distance: 1 = the headset's, 0 = both eyes at the head centre (mono view, head tracking only)\n"
"ipd_scale = 1.0\n"
"# 1 = the view turns with your head (relative to the game camera); 0 = fixed to the game camera\n"
"head_tracking = 1\n"
"# 1 = also follow head position (lean in), 0 = rotation only\n"
"head_position = 1\n"
"# game units per meter (Epic's UE3 convention ~50; The Last Remnant uses cm: 100). Larger = the world looks smaller.\n"
"world_scale = {WORLD_SCALE}\n"
"# A frame is shown in stereo only if it has at least stereo_min_draws 3D draws from a camera at least stereo_min_fov\n"
"# degrees wide (horizontal). Narrower cameras (title-screen backdrops) stay on the flat screen. 0 = no FOV limit.\n"
"stereo_min_draws = {MIN_DRAWS}\n"
"stereo_min_fov = {MIN_FOV}\n"
"# EXPERIMENTAL, keep 0: 1 = draw the right eye once per render pass (FF XIII's batching). In this game it hangs at the\n"
"# title screen (test 019); 0 = right after every left-eye draw (works, ~98 fps at 2748x2880 per eye on an RTX 5070)\n"
"native_batch = 0\n"
"# layer = HUD/minimap/menus on their own panel in front of you; eyes = drawn into both eyes (head-locked)\n"
"hud_mode = layer\n"
"# UI scenes (minimap) on the HUD layer: opacity = brightness x this\n"
"hud_alpha_gain = 1.5\n"
"# The game only draws what its own camera sees (often a narrow 45-90 degrees): with the headset FOV the top and bottom of your view\n"
"# (and things you turn your head to) could be missing. This widens the game's culling by this factor (1 = off).\n"
"cull_fov = {CULL_FOV}\n"
"# HUD panel in front of you (meters)\n"
"hud_distance = 1.5\n"
"hud_width = 1.6\n"
"hud_offset_y = -0.05\n"
"\n"
"# ---- In-game settings menu ----\n"
"# Open / close: hold the pad buttons below (300 ms) or press menu_key. l3r3 = both stick clicks, backstart = Back + Start\n# (View + Menu), ltrt = both\n"
"# triggers, none = keyboard only. menu_key is a virtual-key code (0x2D = Insert).\n"
"menu_chord = l3r3\n"
"menu_hold_ms = 300\n"
"menu_key = 0x2D\n"
"\n"
"# ---- Diagnostics ----\n"
"# Key that captures one frame of draw calls (virtual-key code; 0x7A = F11)\n"
"capture_key = 0x7A\n"
"# Also capture automatically this many seconds after start (0 = off)\n"
"capture_at_sec = 0\n"
"# Save a screenshot of the game image every N seconds to UE3VR\\run_*\\shots\\ (0 = off), at most screenshot_max\n"
"screenshot_interval = 0\n"
"screenshot_max = 30\n"
"# Save the headset images (left | right | HUD) every N seconds after debug_atlas_start seconds (0 = off)\n"
"debug_atlas_interval = 0\n"
"debug_atlas_start = 0\n"
"# Test aid without a headset: extra head yaw / pitch in degrees\n"
"debug_head_yaw = 0\n"
"debug_head_pitch = 0\n"
"# Quit the game automatically after N seconds (0 = never)\n"
"quit_after = 0\n"
"\n"
"# ---- Tweaks (also on the Tweaks page of the settings menu; a game's profile may preset some) ----\n"
"# keep the CPU copy of every game texture for its lifetime (0 = free after upload: black / missing textures in\n"
"# games that update textures later); copies idle for long are dropped above the budget or when address space runs low\n"
"keep_texture_copies = 1\n"
"texture_copy_budget_mb = 1024\n"
"# 1 = frames where a menu is drawn into the 3D scene (UI-scene camera) go to the flat screen\n"
"ui_in_scene_flat = {UI_FLAT}\n"
"# 1 = scene-projection camera draws into shared square textures (floor / water reflections) use the head-tracked\n"
"#     camera instead of the game's (which leaves them stuck to your view)\n"
"shared_target_head = 1\n"
"# eye images from any screen draw that reads the 3D scene (later UE3 post-processing); 0 = only the gamma pass\n"
"composite_any = 1\n"
"\n"
"# Debug switches for finding rendering problems (also on the Debug page of the settings menu)\n"
"# per-eye shadow projection (ScreenToShadowMatrix) / per-eye height fog (ScreenToWorld); 0 = the game's (left-eye) values\n"
"debug_shadow_patch = 1\n"
"debug_fog_patch = 1\n"
"# 1 = skip the modulated-shadow projection draws (no dynamic shadows)\n"
"debug_hide_shadows = 0\n"
"\n"
"# ---- Performance diagnostics (written to GFVR\\gfvr.log every 10 s) ----\n"
"perf_diag = 0\n";

static void LoadSettings() {
    auto& c = G().cfg;
    auto& s = G().s;
    s.vrEnabled = c.GetBool("vr_enabled", true);
    s.logLevel = c.GetInt("log_level", 2);
    s.forceWindowed = c.GetBool("force_windowed", true);
    s.desktopVsyncOff = c.GetBool("desktop_vsync_off", !c.GetBool("desktop_vsync", false));   // menu key / older key
    s.renderScale = c.GetFloat("render_scale", 1.0f);
    s.d3d11HeadsetRes = c.GetBool("d3d11_headset_res", true);
    {
        std::string m = c.GetStr("render_msaa", "game");
        s.renderMsaa = (m == "game" || m.empty()) ? -1 : atoi(m.c_str());
    }
    s.renderWidth = c.GetInt("render_width", 0);
    s.renderHeight = c.GetInt("render_height", 0);
    s.upgradeToEx = c.GetBool("upgrade_to_ex", true);
    s.managedStaging = c.GetStr("managed_textures", "staging") != "dynamic";
    s.screenDistance = c.GetFloat("screen_distance", 2.5f);
    s.screenWidth = c.GetFloat("screen_width", 3.2f);
    s.recenterKey = c.GetInt("recenter_key", VK_F8);
    s.stereo = c.GetBool("stereo", true);
    {
        std::string m = c.GetStr("vr_method", "native");
        for (auto& ch : m) ch = (char)tolower((unsigned char)ch);
        s.vrMethod = m == "aer" ? VR_AER : VR_NATIVE;
    }
    s.fovMode = c.GetInt("fov_mode", 1);
    s.worldScale = c.GetFloat("world_scale", G().profile->worldScale);
    s.ipdScale = c.GetFloat("ipd_scale", 1.0f);
    s.headTracking = c.GetBool("head_tracking", true);
    s.headPosition = c.GetBool("head_position", true);
    s.hudDistance = c.GetFloat("hud_distance", 1.5f);
    s.hudWidth = c.GetFloat("hud_width", 1.6f);
    s.hudOffsetY = c.GetFloat("hud_offset_y", -0.05f);
    s.hudLayer = c.GetStr("hud_mode", "layer") != "eyes";
    s.stereoMinDraws = c.GetInt("stereo_min_draws", G().profile->stereoMinDraws);
    s.stereoMinFov = c.GetFloat("stereo_min_fov", G().profile->stereoMinFov);
    s.uiInSceneFlat = c.GetBool("ui_in_scene_flat", false);
    s.sharedTargetHead = c.GetBool("shared_target_head", true);
    s.d3d11Recon = c.GetBool("d3d11_recon", true);
    s.d3d11Vr = c.GetBool("d3d11_vr", true);
    s.d3d11Stereo = c.GetBool("d3d11_stereo", true);
    s.d3d11HudLayer = c.GetBool("d3d11_hud_layer", true);
    s.d3d11PatchPS = c.GetBool("d3d11_patch_ps", true);
    s.gfNoHeadBob = c.GetBool("no_head_bob", true);
    s.keepJitter = c.GetBool("keep_taa_jitter", true);
    s.prevFramePose = c.GetBool("prev_frame_pose", true);
    s.prevVpCopy = c.GetBool("prev_vp_copy", true);
    s.layoutMerge = c.GetBool("layout_merge", true);
    s.layoutRepair = c.GetBool("layout_repair", true);
    s.debugLayoutPoison = c.GetStr("debug_layout_poison", "");
    s.skipBigCb = c.GetBool("skip_big_cb", true);
    s.lockFree = c.GetBool("lock_free", true);
    s.spinLock = c.GetBool("spin_lock", true);
    s.vrsStrength = c.GetInt("vrs", 0);
    s.vrsInner = c.GetFloat("vrs_inner", 0.45f);
    s.vrsOuter = c.GetFloat("vrs_outer", 0.85f);
    s.vrsFullscreen = c.GetBool("vrs_fullscreen", true);
    s.vrsPerEye = c.GetBool("vrs_per_eye", false);
    s.vrsEyeTracking = c.GetBool("vrs_eye_tracking", false);
    s.vrsSceneOnly = c.GetBool("vrs_scene_only", true);
    s.simGaze = c.GetFloat("sim_gaze", 0.f);
    s.cineCull = c.GetBool("cine_cull_widen", true);
    s.gameDof = c.GetBool("game_dof", false);
    s.eyeMatCache = c.GetBool("eye_mat_cache", true);
    s.drawCamCache = c.GetBool("draw_cam_cache", true);
    s.testLeftOnly = c.GetBool("test_left_only", false);
    s.gfLodDist = c.GetInt("game_lod_distance", -1);
    s.screenshotDiv = c.GetInt("screenshot_div", 4);
    s.xrStartPacing = c.GetBool("xr_start_pacing", true);
    s.gfTaa = (int)c.GetFloat("game_taa", -1);
    s.gfTaaOff = c.GetBool("game_taa_off", false);
    s.gfSsaoQuality = c.GetInt("game_ssao_quality", -1);
    s.gfFxaa = c.GetInt("game_fxaa", -1);
    s.sharpen = c.GetFloat("sharpen", 0.65f);
    s.sharpenClamp = c.GetFloat("sharpen_clamp", 0.035f);
    s.gfOcclusion = c.GetInt("occlusion_mode", 3);
    s.occMargin = c.GetFloat("occlusion_margin", 1.3f);
    s.gfTaaBlend = c.GetFloat("taa_history_blend", -1);
    s.debugHeadSweep = c.GetFloat("debug_head_sweep", 0);
    s.simHmdPitch = c.GetFloat("sim_hmd_pitch", 0);
    s.simHmdYaw = c.GetFloat("sim_hmd_yaw", 0);
    s.simHmdPeriod = c.GetFloat("sim_hmd_period", 4);
    s.simHmdPitchFixed = c.GetFloat("sim_hmd_pitch_fixed", 0);
    s.simHmdPoses = c.GetStr("sim_hmd_poses", "");
    s.simHmdPoseSec = c.GetFloat("sim_hmd_pose_sec", 6);
    s.simHmdPoseStart = c.GetFloat("sim_hmd_pose_start", 0);
    s.simHmdSnapSec = c.GetFloat("sim_hmd_snap_sec", 0);
    s.gfNoCameraShake = c.GetBool("no_camera_shake", true);
    s.gfNoDynamicRes = c.GetBool("no_dynamic_resolution", true);
    s.keepTextureCopies = c.GetBool("keep_texture_copies", true);
    s.textureCopyBudgetMb = c.GetInt("texture_copy_budget_mb", 1024);
    s.dbgShadowPatch = c.GetBool("debug_shadow_patch", true);
    s.dbgFogPatch = c.GetBool("debug_fog_patch", true);
    s.dbgHideShadows = c.GetBool("debug_hide_shadows", false);
    s.compositeAnyTwin = c.GetBool("composite_any", true);
    s.hudAlphaGain = c.GetFloat("hud_alpha_gain", 1.5f);
    s.debugHeadYaw = c.GetFloat("debug_head_yaw", 0);
    s.debugHeadPitch = c.GetFloat("debug_head_pitch", 0);
    s.debugNativeNoSwitch = c.GetBool("debug_native_noswitch", false);
    s.nativeBatch = c.GetBool("native_batch", false);
    s.menuChord = c.GetStr("menu_chord", "l3r3");
    s.menuHoldMs = c.GetInt("menu_hold_ms", 300);
    s.menuKey = c.GetInt("menu_key", VK_INSERT);
    s.captureKey = c.GetInt("capture_key", VK_F11);
    s.captureAtFrame = c.GetInt("capture_at_frame", 0);
    s.captureAtSec = c.GetFloat("capture_at_sec", 0);
    s.captureAtSec2 = c.GetFloat("capture_at_sec2", 0);
    s.pacingDiag = c.GetBool("pacing_diag", false);
    s.perfDiag = c.GetBool("perf_diag", false);
    s.queryNoWait = c.GetBool("query_nowait", false);
    s.frameRate = c.GetInt("frame_rate", 0);
    s.headsetSync = c.GetBool("headset_sync", false);
    s.syncBelow60 = c.GetBool("sync_below_60", false);
    s.reproject = c.GetBool("reproject", false);
    s.cullFov = (float)c.GetFloat("cull_fov", G().profile->cullFov);
#ifdef GFVR
    if (fabsf(s.cullFov - 1.6f) < 0.001f) s.cullFov = G().profile->cullFov;   // old default (builds <= 11): too narrow for headsets
#endif
    s.cullFollowHead = false;   // experiments start off at every launch (a value saved while testing stuck once)
    s.cameraDump = c.GetBool("camera_dump", false);
    s.engineHead = c.GetBool("engine_head_v2", true);   // v2: the D3D11 head pose fix (build 14); old "engine_head" ignored
    s.xrFixLateTime = c.GetBool("xr_fix_late_time", false);
    s.xrOneThread = c.GetBool("xr_one_thread", true);
    s.listCbReupload = c.GetBool("list_cb_reupload", true);
    s.camWatchMs = (int)c.GetFloat("cam_watch_ms", 0);
    s.camCbMaxBytes = (int)c.GetFloat("cam_cb_max_bytes", 16384);
    s.cbDeclFilter = c.GetBool("cb_decl_filter", true);
    s.stereoIsoVs = c.GetStr("stereo_iso_vs", "");
    s.stereoIsoMode = (int)c.GetFloat("stereo_iso_mode", 0);
    s.cullAim = c.GetBool("cull_aim", false);
    s.cullAimCopy = c.GetBool("cull_aim_copy", false);
    s.cullMargin = c.GetFloat("cull_margin", 1.15f);
    s.cullAimTestYaw = c.GetFloat("cull_aim_test_yaw", 0);
    s.cullAimTestPitch = c.GetFloat("cull_aim_test_pitch", 0);
    s.gpuQueueLimit = (int)c.GetFloat("gpu_queue_limit", 0);
    s.hideVsCycle = c.GetStr("hide_vs_cycle", "");
    s.dumpVs = c.GetStr("dump_vs", "");
    s.hideVsSeconds = c.GetFloat("hide_vs_seconds", 5);
    s.hideVsStartSec = c.GetFloat("hide_vs_start_sec", 110);
    s.gfRenderCamCaller = (uint32_t)c.GetFloat("render_cam_caller", 0);
    s.debugSyncPeriodMs = c.GetFloat("debug_sync_period_ms", 0);
    s.screenshotInterval = c.GetFloat("screenshot_interval", 0);
    s.screenshotMax = c.GetInt("screenshot_max", 30);
    s.atlasShotInterval = c.GetFloat("debug_atlas_interval", 0);
    s.atlasShotStart = c.GetFloat("debug_atlas_start", 0);
    s.autoload = c.GetBool("autoload", false);
    s.autoloadScript = c.GetStr("autoload_script", "");
    s.quitAfter = c.GetFloat("quit_after", 0);
}

// ------------------------------------------------------------------ which DLL we are (multi-name proxy)
// The same UEVR3 DLL can be installed as d3d9.dll (default), dinput8.dll (games that don't import d3d9: Life Is Strange,
// Bulletstorm FCE) and later dxgi.dll. Exports of the DLL we are named after are forwarded to the system copy; when
// we are not d3d9.dll, the system d3d9's Direct3DCreate9(Ex) are hooked so the D3D9 path still sees the game.
static wchar_t g_ownName[32] = L"d3d9";   // lower case, no extension
static HMODULE g_real = nullptr, g_realD3D9 = nullptr;
static INIT_ONCE g_realOnce = INIT_ONCE_STATIC_INIT, g_d3d9Once = INIT_ONCE_STATIC_INIT;
static void* g_trampCreate9 = nullptr;     // d3d9 hooked (not proxied): originals of the hooked functions
static void* g_trampCreate9Ex = nullptr;

static void DetectOwnName(HMODULE self) {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(self, p, MAX_PATH);
    const wchar_t* n = wcsrchr(p, L'\\');
    n = n ? n + 1 : p;
    size_t i = 0;
    for (; n[i] && n[i] != L'.' && i < 31; ++i) g_ownName[i] = (wchar_t)towlower(n[i]);
    g_ownName[i] = 0;
}
static bool IsD3D9Proxy() { return !wcscmp(g_ownName, L"d3d9"); }

static HMODULE LoadSystem(const wchar_t* name) {
    wchar_t path[MAX_PATH];
    GetSystemDirectoryW(path, MAX_PATH);   // SysWOW64 for a 32-bit process
    wcscat_s(path, L"\\");
    wcscat_s(path, name);
    wcscat_s(path, L".dll");
    HMODULE m = LoadLibraryW(path);
    if (!m) LOGE("Could not load system %S (%lu)", path, GetLastError());
    return m;
}
static BOOL CALLBACK LoadReal(PINIT_ONCE, PVOID, PVOID*) { g_real = LoadSystem(g_ownName); return TRUE; }
static BOOL CALLBACK LoadRealD3D9(PINIT_ONCE, PVOID, PVOID*) { g_realD3D9 = LoadSystem(L"d3d9"); return TRUE; }

extern "C" HMODULE RealD3D9Module() {
    InitOnceExecuteOnce(&g_d3d9Once, LoadRealD3D9, nullptr, nullptr);
    return g_realD3D9;
}
// A system d3d9 function for our own use (the originals, also when they are hooked because we are not d3d9.dll).
extern "C" FARPROC RealD3D9Proc(const char* name) {
    if (!strcmp(name, "Direct3DCreate9") && g_trampCreate9) return (FARPROC)g_trampCreate9;
    if (!strcmp(name, "Direct3DCreate9Ex") && g_trampCreate9Ex) return (FARPROC)g_trampCreate9Ex;
    HMODULE m = RealD3D9Module();
    FARPROC p = m ? GetProcAddress(m, name) : nullptr;
    if (!p) LOGE("system d3d9.dll has no export %s", name);
    return p;
}
// An export of the DLL we stand in for (forwarded by the pass-through thunks).
extern "C" FARPROC RealOwnProc(const char* name) {
    InitOnceExecuteOnce(&g_realOnce, LoadReal, nullptr, nullptr);
    FARPROC p = g_real ? GetProcAddress(g_real, name) : nullptr;
    if (!p) LOGE("system %S.dll has no export %s", g_ownName, name);
    return p;
}

// Pass-through exports. Resolved on first call through a helper, then a jump with the caller's arguments untouched.
// x86: cdecl helper (the stack arguments stay where they are). x64: the four register arguments (and xmm0-3) are saved
// around the helper call.
#define PASSTHRU_RESOLVE(name, resolver)                                                      \
    extern "C" void* p_##name = nullptr;                                                      \
    extern "C" void* __cdecl Resolve_##name() {                                               \
        if (!p_##name) p_##name = (void*)resolver(#name);                                     \
        return p_##name;                                                                      \
    }
#if defined(_M_IX86)
#define PASSTHRU_AS(name, resolver)                                                           \
    PASSTHRU_RESOLVE(name, resolver)                                                          \
    extern "C" __declspec(naked) void Proxy_##name() { __asm__ volatile("call _Resolve_" #name "\n\tjmp *%eax"); }
#else
// x64 has no naked functions in clang-cl: the thunk is a file-scope assembly function
#define PASSTHRU_AS(name, resolver)                                                                                       \
    PASSTHRU_RESOLVE(name, resolver)                                                                                      \
    extern "C" void Proxy_##name();                                                                                       \
    __asm__(".att_syntax prefix\n\t.text\n\t.globl Proxy_" #name "\n\t.p2align 4\nProxy_" #name ":\n\t"                                          \
            "push %rcx\n\tpush %rdx\n\tpush %r8\n\tpush %r9\n\tsub $0x68, %rsp\n\t"                                       \
            "movdqu %xmm0, 0x20(%rsp)\n\tmovdqu %xmm1, 0x30(%rsp)\n\tmovdqu %xmm2, 0x40(%rsp)\n\tmovdqu %xmm3, 0x50(%rsp)\n\t" \
            "call Resolve_" #name "\n\t"                                                                                  \
            "movdqu 0x20(%rsp), %xmm0\n\tmovdqu 0x30(%rsp), %xmm1\n\tmovdqu 0x40(%rsp), %xmm2\n\tmovdqu 0x50(%rsp), %xmm3\n\t" \
            "add $0x68, %rsp\n\tpop %r9\n\tpop %r8\n\tpop %rdx\n\tpop %rcx\n\tjmp *%rax\n");
#endif
#define PASSTHRU(name) PASSTHRU_AS(name, RealOwnProc)

// d3d9.dll exports (forwarded to the system d3d9 when we are d3d9.dll)
PASSTHRU(D3DPERF_BeginEvent) PASSTHRU(D3DPERF_EndEvent) PASSTHRU(D3DPERF_GetStatus) PASSTHRU(D3DPERF_QueryRepeatFrame)
PASSTHRU(D3DPERF_SetMarker) PASSTHRU(D3DPERF_SetOptions) PASSTHRU(D3DPERF_SetRegion) PASSTHRU(DebugSetLevel)
PASSTHRU(DebugSetMute) PASSTHRU(Direct3D9EnableMaximizedWindowedModeShim) PASSTHRU(Direct3DShaderValidatorCreate9)
PASSTHRU(PSGPError) PASSTHRU(PSGPSampleTexture) PASSTHRU(Direct3DCreate9On12) PASSTHRU(Direct3DCreate9On12Ex)
// dinput8.dll exports (forwarded to the system dinput8 when we are dinput8.dll)
PASSTHRU(DirectInput8Create) PASSTHRU(DllCanUnloadNow) PASSTHRU(DllGetClassObject) PASSTHRU(DllRegisterServer)
PASSTHRU(DllUnregisterServer) PASSTHRU(GetdfDIJoystick)

extern "C" HRESULT WINAPI Proxy_Direct3DCreate9Ex(UINT sdk, IDirect3D9Ex** out);
extern "C" IDirect3D9* WINAPI Proxy_Direct3DCreate9(UINT sdk);
// Not installed as d3d9.dll: the game calls the system d3d9 directly, so its two creation functions are detoured to
// the same code the d3d9.dll exports run (from the init thread: LoadLibrary is not allowed in DllMain).
static void HookSystemD3D9() {
    if (IsD3D9Proxy()) return;
    HMODULE m = RealD3D9Module();
    if (!m) return;
    MH_Initialize();
    void* p = (void*)GetProcAddress(m, "Direct3DCreate9");
    if (p && MH_CreateHook(p, (void*)&Proxy_Direct3DCreate9, &g_trampCreate9) == MH_OK) MH_EnableHook(p);
    p = (void*)GetProcAddress(m, "Direct3DCreate9Ex");
    if (p && MH_CreateHook(p, (void*)&Proxy_Direct3DCreate9Ex, &g_trampCreate9Ex) == MH_OK) MH_EnableHook(p);
    LOGI("loaded as %S.dll: system d3d9 Direct3DCreate9/Ex hooked (%s)", g_ownName, g_trampCreate9 ? "ok" : "failed");
}

extern "C" HRESULT WINAPI Proxy_Direct3DCreate9Ex(UINT sdk, IDirect3D9Ex** out) {
    typedef HRESULT(WINAPI * F)(UINT, IDirect3D9Ex**);
    static F f = (F)RealD3D9Proc("Direct3DCreate9Ex");
    LOGI("Direct3DCreate9Ex(%u) called by the game", sdk);
    OnGameStartingGraphics();   // first D3D use by the game
    SetGameNativeEx();          // it wants Ex itself: don't hide the Ex interfaces from it
    HRESULT hr = f ? f(sdk, out) : E_FAIL;
    if (SUCCEEDED(hr) && *out) OnDirect3DCreated(*out, true);
    return hr;
}

extern "C" IDirect3D9* WINAPI Proxy_Direct3DCreate9(UINT sdk) {
    InstallD3D11Hooks();   // D3D11 UE3 games call Direct3DCreate9 too (renderer probe)
    LOGI("Direct3DCreate9(%u) called by the game", sdk);
    OnGameStartingGraphics();   // first D3D use by the game
    if (G().s.upgradeToEx) {
        typedef HRESULT(WINAPI * FEx)(UINT, IDirect3D9Ex**);
        static FEx fex = (FEx)RealD3D9Proc("Direct3DCreate9Ex");
        IDirect3D9Ex* ex = nullptr;
        if (fex && SUCCEEDED(fex(sdk, &ex)) && ex) {
            LOGI("Upgraded to IDirect3D9Ex %p", ex);
            OnDirect3DCreated(ex, true);
            return ex;   // IDirect3D9Ex derives from IDirect3D9
        }
        LOGE("Direct3DCreate9Ex failed - falling back to plain D3D9 (no VR output possible)");
    }
    typedef IDirect3D9*(WINAPI * F)(UINT);
    static F f = (F)RealD3D9Proc("Direct3DCreate9");
    IDirect3D9* d = f ? f(sdk) : nullptr;
    if (d) OnDirect3DCreated(d, false);
    return d;
}

// ------------------------------------------------------------------ crash logging
// Logs access violations and similar faults (first-chance, so it also sees ones the game handles itself) with the
// faulting module+offset and the return addresses found on the stack. Addresses only; no code is read.
static std::string ModOff(uintptr_t a) {
    HMODULE m = nullptr;
    char buf[MAX_PATH + 32];
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)a, &m) && m) {
        wchar_t p[MAX_PATH];
        GetModuleFileNameW(m, p, MAX_PATH);
        const wchar_t* n = wcsrchr(p, L'\\');
        static HMODULE self = [] {
            HMODULE h = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&RealD3D9Module, &h);
            return h;
        }();
        if (m == self) sprintf_s(buf, "UE3VR+%lx", (unsigned long)(a - (uintptr_t)m));   // this proxy (see dist\d3d9.map)
        else sprintf_s(buf, "%S+%lx", n ? n + 1 : p, (unsigned long)(a - (uintptr_t)m));
    } else sprintf_s(buf, "%08lx", (unsigned long)a);
    return buf;
}

thread_local bool fvr::t_memProbe = false;
static std::string StackHere();

// Crash dump (dbghelp loaded at start-up: at crash time the heap may be unusable). Once per run, into the run folder.
#include <dbghelp.h>
typedef BOOL(WINAPI* MiniDumpWriteDump_t)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION,
                                          PMINIDUMP_CALLBACK_INFORMATION);
static MiniDumpWriteDump_t g_miniDump = nullptr;
static wchar_t g_dumpPath[MAX_PATH] = {};
void PrepareCrashDump() {
    HMODULE m = LoadLibraryW(L"dbghelp.dll");
    if (m) g_miniDump = (MiniDumpWriteDump_t)GetProcAddress(m, "MiniDumpWriteDump");
    swprintf_s(g_dumpPath, L"%scrash.dmp", G().runDir.c_str());
}
static void WriteCrashDump(PEXCEPTION_POINTERS ep) {
    static volatile LONG once = 0;
    if (!g_miniDump || !g_dumpPath[0] || InterlockedExchange(&once, 1)) return;
    HANDLE f = CreateFileW(g_dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), ep, FALSE};
    BOOL ok = g_miniDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                         (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory | MiniDumpWithThreadInfo), &mei, nullptr, nullptr);
    CloseHandle(f);
    LOGE("crash dump %s: %S", ok ? "written" : "FAILED", g_dumpPath);
    xrb::LogFlush();
}

void PrepareCrashDump();
static LONG CALLBACK CrashHandler(PEXCEPTION_POINTERS ep) {
    if (t_memProbe) return EXCEPTION_CONTINUE_SEARCH;   // a memory scan expects faults and handles them itself
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_PRIV_INSTRUCTION && code != 0xC0000409 /*stack buffer overrun*/) {
        // other severe codes (not C++ throws 0xE06D7363, debugger notes 0x4xxxxxxx, single steps of the FOV trap): log once each
        if (code == 0xE06D7363) {   // C++ throw: log those thrown from this proxy (they should never happen)
            static volatile LONG nCpp = 0;
            HMODULE m = nullptr, self = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&CrashHandler, &self);
            // x86 C++ throw: ExceptionInformation[2] = the ThrowInfo, which lives in the throwing module
            HMODULE thrower = nullptr;
            if (ep->ExceptionRecord->NumberParameters >= 3)
                GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCWSTR)ep->ExceptionRecord->ExceptionInformation[2], &thrower);
            if (thrower && thrower == self && InterlockedIncrement(&nCpp) <= 8) {
                std::string st = StackHere();
                LOGE("C++ exception through UEVR3 at frame %ld, thread %lu; stack: %s", G().frame, GetCurrentThreadId(), st.c_str());
                xrb::LogFlush();
            }
            (void)m;
            return EXCEPTION_CONTINUE_SEARCH;
        }
        static DWORD seen[16]; static int nSeen = 0;
        if ((code & 0xF0000000) == 0xC0000000 && code != EXCEPTION_SINGLE_STEP && nSeen < 16) {
            bool known = false;
            for (int i = 0; i < nSeen; ++i) known |= seen[i] == code;
            if (!known) {
                seen[nSeen++] = code;
                LOGW("exception %08lX at %s (frame %ld, thread %lu)", code, ModOff((uintptr_t)ep->ExceptionRecord->ExceptionAddress).c_str(), G().frame,
                     GetCurrentThreadId());
                xrb::LogFlush();
                if (code == 0xC0000374 /* STATUS_HEAP_CORRUPTION */ || code == 0xC0000409 /* STACK_BUFFER_OVERRUN / fail fast */) {
                    WriteCrashDump(ep);   // fatal: the process ends right after this
                    LOGE("  stack: %s", StackHere().c_str());
                    xrb::LogFlush();
                }
            }
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    static volatile LONG count = 0;
    if (InterlockedIncrement(&count) > 8) return EXCEPTION_CONTINUE_SEARCH;
    if (code == EXCEPTION_STACK_OVERFLOW) {   // little stack left: one short line, no stack walk
        static volatile LONG once = 0;
        if (InterlockedExchange(&once, 1) == 0) {
            LOGE("STACK OVERFLOW at %s (thread %lu)", ModOff((uintptr_t)ep->ExceptionRecord->ExceptionAddress).c_str(), GetCurrentThreadId());
            xrb::LogFlush();
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT* c = ep->ContextRecord;
    uintptr_t addr = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    fvr::CrashWatchReport(ep);   // detailed report from a helper thread (registers, stack scan, modules, dump)
    LOGE("EXCEPTION %08lX at %s (frame %ld, thread %lu)%s", code, ModOff(addr).c_str(), G().frame, GetCurrentThreadId(),
         code == EXCEPTION_ACCESS_VIOLATION ? (ep->ExceptionRecord->ExceptionInformation[0] ? " writing" : " reading") : "");
    if (code == EXCEPTION_ACCESS_VIOLATION) LOGE("  address %08lx", (unsigned long)ep->ExceptionRecord->ExceptionInformation[1]);
#if defined(_M_IX86)
    LOGE("  eax %08lx ebx %08lx ecx %08lx edx %08lx esi %08lx edi %08lx ebp %08lx esp %08lx", c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi,
         c->Edi, c->Ebp, c->Esp);
    const uintptr_t sp = c->Esp;
#else
    LOGE("  rax %llx rbx %llx rcx %llx rdx %llx rsi %llx rdi %llx rbp %llx rsp %llx", c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi,
         c->Rbp, c->Rsp);
    const uintptr_t sp = c->Rsp;
#endif
    // return-address candidates: stack values that point into a loaded module
    NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
    std::string line;
    int n = 0;
    for (uintptr_t p = sp; p + sizeof(void*) <= (uintptr_t)tib->StackBase && p < sp + 8192 && n < 24; p += sizeof(void*)) {
        uintptr_t v = *(uintptr_t*)p;
        HMODULE m = nullptr;
        if (v < 0x10000 || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)v, &m)) continue;
        line += ModOff(v) + " ";
        n++;
    }
    LOGE("  stack: %s", line.c_str());
    DumpApiTrace();
    xrb::LogFlush();
    return EXCEPTION_CONTINUE_SEARCH;
}

// return-address candidates on the current stack (exit diagnostics without an exception context)
static std::string StackHere() {
    NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
    uintptr_t esp = (uintptr_t)&tib;
    std::string line;
    int n = 0;
    for (uintptr_t p = esp; p + sizeof(void*) <= (uintptr_t)tib->StackBase && p < esp + 16384 && n < 28; p += sizeof(void*)) {
        uintptr_t v = *(uintptr_t*)p;
        HMODULE m = nullptr;
        if (v < 0x10000 || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)v, &m)) continue;
        line += ModOff(v) + " ";
        n++;
    }
    return line;
}

// The process can end without DLL_PROCESS_DETACH (TerminateProcess on itself, fail-fast, abort): log who did it.
typedef BOOL(WINAPI* TerminateProcess_t)(HANDLE, UINT);
typedef VOID(WINAPI* RaiseFailFast_t)(PEXCEPTION_RECORD, PCONTEXT, DWORD);
static TerminateProcess_t oTerminateProcess;
static RaiseFailFast_t oRaiseFailFast;
static BOOL WINAPI Hk_TerminateProcess(HANDLE h, UINT code) {
    if (h == GetCurrentProcess() || GetProcessId(h) == GetCurrentProcessId()) {
        LOGE("TerminateProcess(self, %u) at frame %ld, thread %lu; stack: %s", code, G().frame, GetCurrentThreadId(), StackHere().c_str());
        DumpApiTrace();
        xrb::LogFlush();
    }
    return oTerminateProcess(h, code);
}
static VOID WINAPI Hk_RaiseFailFast(PEXCEPTION_RECORD er, PCONTEXT ctx, DWORD fl) {
    LOGE("RaiseFailFastException(code %08lX at %s) frame %ld; stack: %s", er ? er->ExceptionCode : 0,
         er ? ModOff((uintptr_t)er->ExceptionAddress).c_str() : "?", G().frame, StackHere().c_str());
    DumpApiTrace();
    xrb::LogFlush();
    oRaiseFailFast(er, ctx, fl);
}
typedef VOID(WINAPI* ExitProcess_t)(UINT);
static ExitProcess_t oExitProcess, oRtlExitUserProcess;
static VOID WINAPI Hk_ExitProcess(UINT code) {
    LOGE("ExitProcess(%u) at frame %ld, thread %lu; stack: %s", code, G().frame, GetCurrentThreadId(), StackHere().c_str());
    DumpApiTrace();
    xrb::LogFlush();
    oExitProcess(code);
}
static VOID WINAPI Hk_RtlExitUserProcess(UINT code) {
    LOGE("RtlExitUserProcess(%u) at frame %ld, thread %lu; stack: %s", code, G().frame, GetCurrentThreadId(), StackHere().c_str());
    DumpApiTrace();
    xrb::LogFlush();
    oRtlExitUserProcess(code);
}
static void OnAbort(int) {
    LOGE("abort() at frame %ld, thread %lu; stack: %s", G().frame, GetCurrentThreadId(), StackHere().c_str());
    DumpApiTrace();
    xrb::LogFlush();
}
static void OnTerminate() {
    LOGE("std::terminate at frame %ld, thread %lu; stack: %s", G().frame, GetCurrentThreadId(), StackHere().c_str());
    DumpApiTrace();
    xrb::LogFlush();
    abort();
}
static void InstallExitDiagnostics() {
    static bool init = [] { MH_STATUS s = MH_Initialize(); return s == MH_OK || s == MH_ERROR_ALREADY_INITIALIZED; }();
    (void)init;
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    void* p = (void*)GetProcAddress(k, "TerminateProcess");
    if (p && MH_CreateHook(p, (void*)&Hk_TerminateProcess, (void**)&oTerminateProcess) == MH_OK) MH_EnableHook(p);
    p = (void*)GetProcAddress(k, "RaiseFailFastException");
    if (p && MH_CreateHook(p, (void*)&Hk_RaiseFailFast, (void**)&oRaiseFailFast) == MH_OK) MH_EnableHook(p);
    p = (void*)GetProcAddress(k, "ExitProcess");
    if (p && MH_CreateHook(p, (void*)&Hk_ExitProcess, (void**)&oExitProcess) == MH_OK) MH_EnableHook(p);
    p = (void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlExitUserProcess");
    if (p && MH_CreateHook(p, (void*)&Hk_RtlExitUserProcess, (void**)&oRtlExitUserProcess) == MH_OK) MH_EnableHook(p);
    signal(SIGABRT, OnAbort);
    std::set_terminate(OnTerminate);
}

// ------------------------------------------------------------------ startup
static void Init() {
    G().startTick = GetTickCount();
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir = path;
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
    G().dllDir = dir;
    {
        std::wstring exe = path;
        exe = exe.substr(exe.find_last_of(L"\\/") + 1);
        for (auto& ch : exe) ch = (wchar_t)towlower(ch);
        G().exeName = exe;
        G().profile = &DetectProfile(exe.c_str());
        G().game = GAME_UE3;
        G().gameName = G().profile->exe[0] ? G().profile->name : "unknown UE3 game (generic profile)";
    }
    G().outDir = dir + L"GFVR\\";
    CreateDirectoryW(G().outDir.c_str(), nullptr);
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t run[64];
    swprintf_s(run, L"run_%04d%02d%02d_%02d%02d%02d\\", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    G().runDir = G().outDir + run;
    CreateDirectoryW(G().runDir.c_str(), nullptr);

    std::wstring cfgPath = dir + L"gfvr_config.txt";
    {
        std::string t = kDefaultConfig;
        auto rep = [&](const char* k, const std::string& v) { size_t p; while ((p = t.find(k)) != std::string::npos) t.replace(p, strlen(k), v); };
        char b[32];
        rep("{GAME}", G().profile->exe[0] ? G().profile->name : "generic Unreal Engine 3 profile");
        sprintf_s(b, "%.0f", G().profile->worldScale); rep("{WORLD_SCALE}", b);
        sprintf_s(b, "%.2f", G().profile->cullFov); rep("{CULL_FOV}", b);
        sprintf_s(b, "%d", G().profile->stereoMinDraws); rep("{MIN_DRAWS}", b);
        sprintf_s(b, "%.0f", G().profile->stereoMinFov); rep("{MIN_FOV}", b);
        rep("{UI_FLAT}", std::string(G().profile->tweaks).find("ui_in_scene_flat=1") != std::string::npos ? "1" : "0");
        xrb::WriteDefaultConfig(cfgPath.c_str(), t.c_str());
    }
    G().cfg.Load(cfgPath.c_str());
    {   // the profile's tweaks are defaults: a value in ue3vr_config.txt wins
        std::string t = G().profile->tweaks ? G().profile->tweaks : "";
        size_t p = 0;
        while (p < t.size()) {
            size_t e = t.find(' ', p);
            std::string kv = t.substr(p, e == std::string::npos ? std::string::npos : e - p);
            size_t eq = kv.find('=');
            if (eq != std::string::npos) G().cfg.SetDefault(kv.substr(0, eq).c_str(), kv.substr(eq + 1).c_str());
            if (e == std::string::npos) break;
            p = e + 1;
        }
    }
    LoadSettings();
    xrb::LogInit((G().outDir + L"gfvr.log").c_str(), G().s.logLevel);
    LOGI("GreedFall VR " GFVR_VERSION " (gfvr, UEVR3 core) build " UE3VR_BUILD_ID " loaded into %S (base %p), game: %s", path, (void*)GameModule().base, G().gameName);
    LOGI("profile: %s (world_scale %.0f, cull_fov %.2f, stereo_min_draws %d) - %s", G().profile->name, G().profile->worldScale, G().profile->cullFov, G().profile->stereoMinDraws, G().profile->notes);
    G().cfg.Dump();
    LOGI("output of this run: %S", G().runDir.c_str());
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        DetectOwnName(h);
        Init();
        LOGI("loaded as %S.dll (%s)", g_ownName, sizeof(void*) == 8 ? "x64" : "x86");
        AddVectoredExceptionHandler(0, CrashHandler);
        fvr::InstallCrashWatch();
        fvr::StartModuleWatch();
        InstallExitDiagnostics();
        InstallInputHooks();
        InstallGreedfallFixes();
        // D3D11 recon hooks need LoadLibrary (not allowed in DllMain): from a thread that runs right after the loader
        if (HANDLE t = CreateThread(nullptr, 0, [](LPVOID) -> DWORD { PrepareCrashDump(); HookSystemD3D9(); InstallD3D11Hooks(); return 0; }, nullptr, 0, nullptr)) CloseHandle(t);   // dinput8 is a static import of the game: already mapped, safe to detour
    } else if (reason == DLL_PROCESS_DETACH) {
        LOGI("process exit (frame %ld)", G().frame);
        DumpApiTrace();
        xrb::ClientShutdown();
        LOGI("Unloading");
        xrb::LogFlush();
    }
    return TRUE;
}
