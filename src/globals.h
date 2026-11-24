#pragma once
#define GFVR_VERSION "1.0-rc1"
#include <windows.h>
#include <string>
#include "log.h"
#include "config.h"

namespace fvr {

enum VrMethod { VR_AER = 0, VR_NATIVE = 1 };
// Which game loaded us (from the exe name). One DLL for the FF XIII trilogy; per-game differences key off this.
enum Game { GAME_UNKNOWN = 0, GAME_FF13 = 1, GAME_FF13_2 = 2, GAME_LR = 3, GAME_UE3 = 4 };
struct Profile;

struct Settings {
    // general
    bool vrEnabled = true;
    int logLevel = 2;
    // display / device
    bool forceWindowed = true;          // run the game in a window (robust, needed for unattended tests)
    bool desktopVsyncOff = true;        // force PresentationInterval IMMEDIATE (desktop_vsync = 0)
    float renderScale = 1.0f;           // scene resolution = headset per-eye size x this (0 = the game's own resolution)
    int renderWidth = 0, renderHeight = 0;   // >0: explicit scene resolution instead (no headset needed)
    int renderMsaa = -1;                // -1 = as the game asks (4x), else 0/2/4/8 samples for the resized scene targets
    bool upgradeToEx = true;            // D3D9 -> D3D9Ex (required for shared textures / VR output)
    bool managedStaging = true;         // managed textures: DEFAULT + staging on lock (saves address space) vs DEFAULT+DYNAMIC
    // virtual screen (milestone 1: flat game image on a panel in the headset)
    float screenDistance = 2.5f, screenWidth = 3.2f;
    int recenterKey = VK_F8;            // recenter the headset view
    // stereo (milestone 2)
    bool stereo = true;                 // 3D scene in stereo; menus/movies stay on the flat screen
    int vrMethod = VR_NATIVE;           // native: both eyes every frame (twin targets); aer: alternate-eye frames
    int fovMode = 1;                    // 1 = headset FOV (immersive), 0 = game FOV (a window; nothing can be culled)
    float worldScale = 100.0f;          // game units per meter (UE3 here: 1 unit = 1 cm)
    float ipdScale = 1.0f;              // 0 = both eyes at the head centre (mono), 1 = the headset's eye distance
    bool headTracking = true;           // eyes follow the headset's rotation (relative to the game camera)
    bool headPosition = true;           // and its position
    float hudDistance = 1.5f, hudWidth = 1.6f, hudOffsetY = -0.05f;
    bool compositeAnyTwin = true;       // eye images: any back-buffer draw sampling a twinned target counts (not only the gamma pass)
    float stereoMinFov = 0;             // cameras narrower than this (horizontal degrees) are not patched / not 3D scenes
    bool d3d11Recon = true;
    bool d3d11PatchPS = true;           // debug: per-eye camera values in pixel-shader constants (deferred passes)
    bool d3d11Stereo = true;
    bool d3d11HeadsetRes = true;        // menu (Resolution, restart): the game renders at the headset's per-eye size x render_scale
    bool d3d11HudLayer = true;          // D3D11: UI after the scene composite -> HUD layer (0: drawn into both eyes)            // D3D11 games: stereo 3D (per-draw eye constants, right-eye twins); off = flat screen
    float simHmdPitch = 0, simHmdYaw = 0, simHmdPeriod = 4, simHmdPitchFixed = 0;
    std::string simHmdPoses;            // test aid: "yaw,pitch[,cull];..." fixed poses, sim_hmd_pose_sec each, one atlas per pose
    float simHmdPoseSec = 6, simHmdPoseStart = 0, simHmdSnapSec = 0;   // start: seconds after the first frame   // test aid: simulated headset motion (OpenXR poses)
    float debugHeadSweep = 0;           // test aid: oscillating head yaw amplitude (deg)
    bool xrStartPacing = true;          // xrWaitFrame/xrBeginFrame right after Present (start of the next frame)
    bool prevFramePose = true;
    bool prevVpCopy = true;
    bool layoutMerge = true;
    bool layoutRepair = true;
    bool mousePointer = false;          // menu (HUD): draw the game's mouse pointer on the menu / HUD panel while Windows shows it
    float mousePointerSize = 0.5f;      // menu (HUD): pointer size relative to a 1080p screen           // menu (Debug): a VS that took the previous frame's VP as its camera is switched to the current one
    std::string debugLayoutPoison;      // test aid: VS hash whose first layout search skips rows 0-11
    bool skipBigCb = true;
    bool lockFree = true;
    int vrsStrength = 0;                // menu: VRS foveated rendering 0 off, 1 gentle, 2 balanced, 3 strong (NVIDIA only)
    float vrsInner = 0.45f, vrsOuter = 0.85f;   // menu: full-rate / middle ring radii (fraction of half the eye height)
    bool vrsFullscreen = true;
    bool vrsEyeTracking = false;         // menu (Display): the full-rate area follows the eye tracker when there is one
    bool vrsSceneOnly = true;           // menu (Debug): VRS only into 3D scene targets (never the UI texture)
    float simGaze = 0;                  // test aid sim_gaze: simulated gaze circles this many degrees
    bool vrsPerEye = false;             // menu (Debug): one image per eye centre (an extra state change per stereo draw)          // menu: VRS also on full-screen passes without depth (lighting, effects)
    bool cineCull = true;               // menu: widen narrow (cinematic, < 20 deg) render cameras for culling too
    bool gameDof = false;               // menu: the game's depth of field (RENDER\\EnableDOF); off = forced off every frame
    bool eyeMatCache = true;            // menu: per-context cache of the eye matrices (one global lock less per draw)
    bool drawCamCache = true;           // menu: reuse the previous draw's camera check when nothing it depends on changed
    bool testLeftOnly = false;          // test aid (menu): right eye not drawn (measures the cost of the second draw)
    int gfLodDist = -1;                 // test aid: force the game's OPTION\\LodDistanceQuality (-1 = game's own)
    bool spinLock = true;               // menu: the mod's shared lock spins briefly instead of sleeping               // menu: deferred contexts update their buffer copies without the global lock              // menu: don't keep CPU copies of constant buffers > cam_cb_max_bytes (never patched since build 14)            // menu: complete camera layouts of the same buffer from other shaders
    int screenshotDiv = 4;              // test aid: atlas BMP downscale (screenshot_div)             // menu: also patch a second copy of the previous-frame VP (GreedFall motion vectors read it)
    float occMargin = 1.3f;             // occlusion_mode 3: eye NDC bound beyond which objects count as hidden
    int gfOcclusion = 3;                // menu: the game's occlusion depth readback: 0 = as rendered (left eye), 1 = re-mapped to the game's camera, 2 = empty (no occlusion culling)
    bool gfTaaOff = false;
    int gfSsaoQuality = -1;
    int gfFxaa = -1;                    // test aid: force the game's OPTION\\FXAA (-1 = game's own)
    float sharpen = 0.65f;              // menu: LumaSharpen strength on the headset image (0 = off)
    float sharpenClamp = 0.035f;        // menu: largest change per pixel             // test aid: force the game's OPTION\\SSAOQuality (-1 = game's own)              // menu: turn the game's TAA off (restores the game's own value when turned back)
    int gfTaa = -1;                     // game's TAA: -1 leave the game's option, 0 off, 1 on
    float gfTaaBlend = -1;              // game's TAA history blend factor (-1 = leave)          // previous-frame matrices with last frame's head pose (TAA / motion vectors)
    bool keepJitter = true;            // keep the game's TAA jitter in the headset projection
    bool gfNoHeadBob = true;           // GreedFall: camera head bob removed
    bool gfNoCameraShake = true;       // GreedFall: all camera shakes off (game's own debug switch)
    bool gfNoDynamicRes = true;        // GreedFall: dynamic resolution forced off
    bool d3d11Vr = true;                // D3D11 games: the game image on the flat screen in the headset (stage 2)             // D3D11 games: observe and log (stage 1 of the D3D11 backend)
    bool uiInSceneFlat = false;
    bool sharedTargetHead = true;       // tweak: scene-projection camera draws into shared (square) targets follow the head         // tweak: frames with a UI scene drawn into the 3D scene stay flat
    bool keepTextureCopies = true;      // tweak: keep the CPU copy of emulated MANAGED textures for their lifetime
    int textureCopyBudgetMb = 1024;     // above this (or < 192 MB free address space) long-idle copies are evicted
    bool dbgShadowPatch = true;         // debug: per-eye ScreenToShadowMatrix (modulated shadows)
    bool dbgFogPatch = true;            // debug: per-eye ScreenToWorld (height fog)
    bool dbgHideShadows = false;        // debug: skip the modulated-shadow projection draws
    int stereoMinDraws = 12;            // a frame is shown in stereo only with at least this many 3D draws (not the title)
    bool hudLayer = true;               // UI (minimap, menus) on its own headset layer; false = drawn into both eyes
    float hudAlphaGain = 1.5f;          // UI scenes on the HUD layer: alpha = brightest channel x this
    float debugHeadYaw = 0;             // test aid: extra head yaw in degrees
    float debugHeadPitch = 0;
    bool debugNativeNoSwitch = false;   // timing test only: native right-eye draws without switching targets (wrong image)
    bool nativeBatch = true;            // native: draw the right eye per render pass in one batch (stereobatch.cpp)
    // in-game settings menu (menu.cpp, vrkit)
    std::string menuChord = "l3r3";     // l3r3 | ltrt | none
    int menuHoldMs = 300;
    int menuKey = VK_INSERT;
    // diagnostics
    int captureKey = VK_F11;            // one-frame draw capture
    int captureAtFrame = 0;             // >0: also capture automatically at this frame number
    float captureAtSec = 0, captureAtSec2 = 0;   // >0: capture once this many seconds after start
    bool pacingDiag = false;            // log how the render thread spends each frame (Sleep/waits/Present)
    bool perfDiag = false;              // log GPU busy time / render-thread CPU time per frame
    bool queryNoWait = false;           // the game's EVENT queries report "done" at once (lets CPU and GPU overlap)
    int frameRate = 0;                  // >0 and != 60: target frame rate of the game's frame limiter (it runs at 60)
    std::string dumpVs;                 // test aid: vertex shaders to dump (hashes, or *)
    std::string hideVsCycle; float hideVsSeconds = 5, hideVsStartSec = 110;   // test aid (d3d11hooks.cpp)
    bool engineHead = false;            // (default off: part of the engine kept the unrotated camera -> stretched geometry, test 047)
    bool xrFixLateTime = false;          // replace OpenXR display times that are already in the past (not needed with xr_one_thread)
    bool xrOneThread = true;
    bool listCbReupload = true;
    int camWatchMs = 0;
    bool cbDeclFilter = true;           // only the constant buffers a shader declares (not dynamically indexed) are patched
    int camCbMaxBytes = 16384;          // constant buffers larger than this are never searched / patched for a camera
    std::string stereoIsoVs; int stereoIsoMode = 0;   // test aid: one VS without camera patch (1) / left eye only (2)
    float cullMargin = 1.15f;           // culling view = headset FOV x this (game camera follows head)
    bool cullAimCopy = false;           // experiment: turn the camera snapshot copy (Engine.dll+0x567270)
    bool cullAim = false;               // culling camera turns with the head (head-aimed culling)
    float cullAimTestYaw = 0, cullAimTestPitch = 0;   // test aid: fixed extra turn of the culling camera                 // test aid: read watchpoints on the gameplay camera node for N ms         // every command list uploads its own per-eye camera buffers (fix: one-frame streaks)            // all OpenXR calls on one thread of ours (SteamVR breaks when they come from changing threads)
    int gpuQueueLimit = 0;              // CPU waits until the GPU finished the frame N back (0 = off)          // replace OpenXR display times that are already in the past
    uint32_t gfRenderCamCaller = 0;     // extra setFOV call site (Engine.dll offset) treated as the render camera (test aid)             // turn the game's render camera with the head (GreedFall camera hook)
    bool cameraDump = false;            // RE aid: dump the render camera's memory (greedfall.cpp)
    bool cullFollowHead = false;        // culling FOV widens with the head angle (off: > ~120 deg breaks LOD / shadows / effects)         // culling FOV widens with the head's angle away from the game camera
    float cullFov = 1.0f;               // >1: widen the game camera's culling FOV by this factor (camfov.cpp); 1 = off
    bool reproject = false;              // camera-aware reprojection: the host turns each image with the game camera between frames
    bool syncBelow60 = false;           // phase lock also below 60 Hz (e.g. OFXR 3X: host at 30 Hz gets every 2nd frame, evenly)
    bool headsetSync = true;            // headset at a multiple of 60 Hz: show each game frame for exactly N headset frames
    float debugSyncPeriodMs = 0;        // test aid: pretend the headset period is this (and visible)
    float screenshotInterval = 0;       // seconds between backbuffer screenshots (0 = off)
    float atlasShotInterval = 0, atlasShotStart = 0;   // headset atlas images every N s from start+M s (0 = off)
    int screenshotMax = 30;
    // input / automation
    bool autoload = false;
    std::string autoloadScript;
    float quitAfter = 0;                // seconds after start; 0 = never (test automation)
};

struct Globals {
    std::wstring dllDir;                // folder of d3d9.dll (= game exe folder)
    std::wstring outDir;                // dllDir\FF13VR\  (logs, dumps, captures)
    std::wstring runDir;                // outDir\run_<date>_<time>\  (screenshots and captures of this run)
    xrb::Config cfg;
    Settings s;
    bool engineHeadHooked = false;      // the game camera hook is in place (engine head mode possible)
    DWORD startTick = 0;
    HWND gameWnd = nullptr;
    volatile LONG frame = 0;
    int game = GAME_UNKNOWN;
    const char* gameName = "unknown game";
    std::wstring exeName;               // lower case
    const Profile* profile = nullptr;   // never null after Init (profiles.cpp)
};
Globals& G();

float SecondsSinceStart();

// Set on threads that read memory which may vanish (camfov's scan): the crash logger ignores their faults.
extern thread_local bool t_memProbe;

} // namespace fvr
