// OpenXR inside the game process (64-bit D3D11 games): replaces XrBridgeHost.exe + shared textures.
// Implements the xrb client API (include/xrbridge/client.h) that the rest of the mod already uses, plus XrFrame11()
// which runs the whole OpenXR frame at the game's Present on the game's own D3D11 device (pattern of DAI VR's xr.cpp):
//   poll events -> xrWaitFrame -> xrBeginFrame -> locate views (poses for the NEXT rendered frame)
//   -> copy this frame's eye images / panel into swapchains -> projection layer with the poses they were rendered
//   with + HUD quad (or the flat-screen quad) -> xrEndFrame.
// The D3D9 texture-ring functions of the API are stubs here (GFVR is D3D11 only).
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <vector>
#include <string>
#include <cstdio>
#include <cstring>
#include <cmath>
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "xrbridge/client.h"
#include "xr_inproc.h"
#include "probe11.h"
#include "log.h"

namespace xrb {

static ClientSettings g_cs;
static XrInstance g_inst = XR_NULL_HANDLE;
static XrSystemId g_sys = XR_NULL_SYSTEM_ID;
static volatile LONG g_instReady = 0, g_initStarted = 0;
static bool g_failed = false;
static uint32_t g_recW = 0, g_recH = 0;
static std::string g_runtime, g_system;

static XrSession g_sess = XR_NULL_HANDLE;
static XrSpace g_local = XR_NULL_HANDLE, g_view = XR_NULL_HANDLE, g_ref = XR_NULL_HANDLE;
static XrSessionState g_state = XR_SESSION_STATE_UNKNOWN;
static bool g_running = false;
static volatile LONG g_recenterReq = 1;   // recentre once the head is tracked
static int64_t g_period = 0;
static uint32_t g_frames = 0;

static IpcPose g_pose[2];
// Simulated headset motion (test aid, no real HMD needed): rotates the poses OpenXR reports, so rendering AND the pose
// submitted to the compositor see it exactly like real head motion. Pitch (look down/up) and yaw, amplitude deg, period s.
static float g_simPitch = 0, g_simYaw = 0, g_simPeriod = 4, g_simPitchFixed = 0, g_simYawFixed = 0;
void XrSetSimulatedHead(float pitchDeg, float yawDeg, float periodSec, float fixedPitchDeg, float fixedYawDeg) { g_simPitch = pitchDeg; g_simYaw = yawDeg; g_simPeriod = periodSec > 0.2f ? periodSec : 4; g_simPitchFixed = fixedPitchDeg; g_simYawFixed = fixedYawDeg; }
static void QMul(const float a[4], const float b[4], float o[4]) {   // xyzw
    o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}
static void QRot(const float q[4], const float v[3], float o[3]) {
    float p[4] = {v[0], v[1], v[2], 0}, qi[4] = {-q[0], -q[1], -q[2], q[3]}, t[4], r[4];
    QMul(q, p, t); QMul(t, qi, r);
    o[0] = r[0]; o[1] = r[1]; o[2] = r[2];
}
static void SimulateHead(IpcPose pose[2], XrTime t) {
    if (g_simPitch == 0 && g_simYaw == 0 && g_simPitchFixed == 0 && g_simYawFixed == 0) return;
    double sec = (double)t * 1e-9;
    float ph = (float)(sec / g_simPeriod * 6.283185307);
    float pitch = (g_simPitchFixed + g_simPitch * std::sin(ph)) * 0.0174533f, yaw = (g_simYawFixed + g_simYaw * std::sin(ph * 0.5f)) * 0.0174533f;
    float qy[4] = {0, std::sin(yaw * 0.5f), 0, std::cos(yaw * 0.5f)}, qp[4] = {std::sin(pitch * 0.5f), 0, 0, std::cos(pitch * 0.5f)}, q[4];
    QMul(qy, qp, q);
    // rotate about the midpoint between the eyes (the head), as a real head turn does
    float c[3] = {(pose[0].px + pose[1].px) * 0.5f, (pose[0].py + pose[1].py) * 0.5f, (pose[0].pz + pose[1].pz) * 0.5f};
    for (int e = 0; e < 2; ++e) {
        float d[3] = {pose[e].px - c[0], pose[e].py - c[1], pose[e].pz - c[2]}, r[3];
        QRot(q, d, r);
        pose[e].px = c[0] + r[0]; pose[e].py = c[1] + r[1]; pose[e].pz = c[2] + r[2];
        float o[4] = {pose[e].qx, pose[e].qy, pose[e].qz, pose[e].qw}, n[4];
        QMul(q, o, n);
        pose[e].qx = n[0]; pose[e].qy = n[1]; pose[e].qz = n[2]; pose[e].qw = n[3];
    }
}
// ---- eye tracking (XR_EXT_eye_gaze_interaction) for foveated rendering. Gaze = a direction in g_ref space.
static bool g_gazeExt = false, g_gazeSys = false, g_gazeReady = false;
static XrActionSet g_gazeSet = XR_NULL_HANDLE; static XrAction g_gazeAct = XR_NULL_HANDLE; static XrSpace g_gazeSpace = XR_NULL_HANDLE;
static volatile bool g_gazeWant = false;
static float g_gazeDir[3] = {0, 0, -1}; static bool g_gazeValid = false; static uint32_t g_gazeFrame = 0;
static uint64_t g_gazeOk = 0, g_gazeLost = 0;
static float g_simGaze = 0, g_simGazePeriod = 3;   // test aid: the gaze circles this many degrees around the view centre
void XrSetEyeTracking(bool on) { g_gazeWant = on; }
void XrSetSimulatedGaze(float amplitudeDeg, float periodSec) { g_simGaze = amplitudeDeg; g_simGazePeriod = periodSec > 0.2f ? periodSec : 3; }
static bool XrOk(XrResult r, const char* what);
static void SetupGaze() {
    XrActionSetCreateInfo ai{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy_s(ai.actionSetName, "gfvr_gaze"); strcpy_s(ai.localizedActionSetName, "Eye tracking");
    if (!XrOk(xrCreateActionSet(g_inst, &ai, &g_gazeSet), "xrCreateActionSet")) return;
    XrActionCreateInfo ac{XR_TYPE_ACTION_CREATE_INFO};
    ac.actionType = XR_ACTION_TYPE_POSE_INPUT;
    strcpy_s(ac.actionName, "eye_gaze"); strcpy_s(ac.localizedActionName, "Eye gaze");
    if (!XrOk(xrCreateAction(g_gazeSet, &ac, &g_gazeAct), "xrCreateAction")) return;
    XrPath prof = 0, bind = 0;
    xrStringToPath(g_inst, "/interaction_profiles/ext/eye_gaze_interaction", &prof);
    xrStringToPath(g_inst, "/user/eyes_ext/input/gaze_ext/pose", &bind);
    XrActionSuggestedBinding sb{g_gazeAct, bind};
    XrInteractionProfileSuggestedBinding is{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    is.interactionProfile = prof; is.suggestedBindings = &sb; is.countSuggestedBindings = 1;
    if (!XrOk(xrSuggestInteractionProfileBindings(g_inst, &is), "xrSuggestInteractionProfileBindings")) return;
    XrSessionActionSetsAttachInfo at{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    at.countActionSets = 1; at.actionSets = &g_gazeSet;
    if (!XrOk(xrAttachSessionActionSets(g_sess, &at), "xrAttachSessionActionSets")) return;
    XrActionSpaceCreateInfo sc{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    sc.action = g_gazeAct; sc.poseInActionSpace.orientation.w = 1;
    if (!XrOk(xrCreateActionSpace(g_sess, &sc, &g_gazeSpace), "xrCreateActionSpace")) return;
    g_gazeReady = true;
    LOGI("xr: eye tracking ready");
}
static void UpdateGaze(XrTime t) {
    if (g_simGaze != 0) {   // circle around the (simulated) head's forward direction
        float ph = (float)((double)t * 1e-9 / g_simGazePeriod * 6.283185307), a = g_simGaze * 0.0174533f;
        float d[3] = {std::sin(a * std::cos(ph)), std::sin(a * std::sin(ph)), 0}; d[2] = -std::sqrt(std::max(0.f, 1 - d[0] * d[0] - d[1] * d[1]));
        float q[4] = {g_pose[0].qx, g_pose[0].qy, g_pose[0].qz, g_pose[0].qw};
        QRot(q, d, g_gazeDir); g_gazeValid = true; g_gazeFrame = g_frames; ++g_gazeOk;
        return;
    }
    if (!g_gazeWant || !g_gazeReady || g_state != XR_SESSION_STATE_FOCUSED) return;
    XrActiveActionSet as{g_gazeSet, XR_NULL_PATH};
    XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO}; si.countActiveActionSets = 1; si.activeActionSets = &as;
    if (XR_FAILED(xrSyncActions(g_sess, &si))) { ++g_gazeLost; return; }
    XrActionStatePose ps{XR_TYPE_ACTION_STATE_POSE}; XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO}; gi.action = g_gazeAct;
    xrGetActionStatePose(g_sess, &gi, &ps);
    XrEyeGazeSampleTimeEXT st{XR_TYPE_EYE_GAZE_SAMPLE_TIME_EXT};
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION}; loc.next = &st;
    if (ps.isActive && XR_SUCCEEDED(xrLocateSpace(g_gazeSpace, g_ref, t, &loc)) && (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) &&
        (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT)) {
        const XrQuaternionf& o = loc.pose.orientation;
        float q[4] = {o.x, o.y, o.z, o.w}, f[3] = {0, 0, -1};
        QRot(q, f, g_gazeDir); g_gazeValid = true; g_gazeFrame = g_frames; ++g_gazeOk;
    } else ++g_gazeLost;
}
// gaze direction (g_ref space) of the last couple of frames; status: -1 runtime lacks it, -2 headset lacks it, 0 starting, 1 ready, 2 tracking
bool XrGetGaze(float dir[3]) {
    if (!g_gazeValid || g_frames - g_gazeFrame > 3) return false;
    memcpy(dir, g_gazeDir, sizeof g_gazeDir);
    return true;
}
int XrEyeTrackingStatus() {
    if (g_simGaze != 0) return 2;
    if (g_gazeValid && g_frames - g_gazeFrame <= 3) return 2;
    if (!g_instReady) return 0;
    if (!g_gazeExt) return -1;
    if (!g_gazeSys) return -2;
    if (!g_gazeReady) return 0;
    return g_gazeValid && g_frames - g_gazeFrame <= 3 ? 2 : 1;
}
static IpcFov g_fov[2];
static int64_t g_poseTime = 0;
static bool g_poseOk = false;

static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static std::vector<int64_t> g_fmts;

struct Swap {
    XrSwapchain sc = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> img;
    uint32_t w = 0, h = 0;
    int64_t fmt = 0;
    DXGI_FORMAT src = DXGI_FORMAT_UNKNOWN;
    bool failed = false;
};
static Swap g_eye[2], g_panel;
static XrFrameState g_fs{XR_TYPE_FRAME_STATE};
static bool g_begun = false;     // xrWaitFrame + xrBeginFrame done, xrEndFrame pending
static uint64_t g_statPresents = 0, g_statXr = 0, g_statLayers[3] = {};
static double g_statWait = 0;
// frame timing diagnostics (slideshow): display times handed out vs real time
static PFN_xrConvertWin32PerformanceCounterToTimeKHR g_qpcToXr = nullptr;
static XrTime XrNow() {
    if (!g_qpcToXr || !g_inst) return 0;
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    XrTime t = 0;
    return XR_SUCCEEDED(g_qpcToXr(g_inst, &q, &t)) ? t : 0;
}
static XrTime g_prevDisplay = 0;
// Late display time fix: SteamVR handed out predicted display times 200-300 ms in the PAST (test 047: every frame
// "late", xrWaitFrame never waiting) -> frames tagged as long overdue. When that happens the frame gets its own target:
// now + expected time to render it (average frame interval) + one display period - for the poses and for xrEndFrame.
static bool g_fixLateTime = true;
static double g_frameEma = 0;          // ms between frame starts
static XrTime g_lastBegin = 0;
static uint64_t g_stFixed = 0;
void XrSetFixLateTime(bool on) { g_fixLateTime = on; }
static double g_stLeadWait = 0, g_stLeadEnd = 0, g_stStep = 0, g_stPeriod = 0, g_stAcquire = 0, g_stEnd = 0;
static uint64_t g_stN = 0, g_stLate = 0, g_stSame = 0, g_stNoRender = 0, g_stEndN = 0;
static DWORD g_statTick = 0;

static const char* Res(XrResult r) {
    static char b[64];
    if (g_inst && xrResultToString(g_inst, r, b) == XR_SUCCESS) return b;
    snprintf(b, sizeof b, "XrResult %d", (int)r);
    return b;
}

static bool XrOk(XrResult r, const char* what) {
    if (XR_SUCCEEDED(r)) return true;
    LOGW("xr: eye tracking: %s: %s - eye tracking off", what, Res(r));
    return false;
}
// ------------------------------------------------------------------ instance + system (worker thread)
static bool CreateInstance() {
    uint32_t n = 0;
    if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr))) { LOGW("xr: no OpenXR runtime (is SteamVR installed?)"); return false; }
    std::vector<XrExtensionProperties> ext(n, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, n, &n, ext.data());
    bool d3d11 = false;
    for (auto& e : ext) if (!strcmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME)) d3d11 = true;
    if (!d3d11) { LOGE("xr: the OpenXR runtime has no %s", XR_KHR_D3D11_ENABLE_EXTENSION_NAME); g_failed = true; return false; }
    bool qpcTime = false;
    for (auto& e : ext) if (!strcmp(e.extensionName, XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME)) qpcTime = true;
    for (auto& e : ext) if (!strcmp(e.extensionName, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME)) g_gazeExt = true;
    std::vector<const char*> exts = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    if (qpcTime) exts.push_back(XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME);
    if (g_gazeExt) exts.push_back(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
    LOGI("xr: eye tracking (%s) %s by the runtime", XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME, g_gazeExt ? "offered" : "NOT offered");
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    strncpy_s(ci.applicationInfo.applicationName, g_cs.appName.c_str(), _TRUNCATE);
    ci.applicationInfo.applicationVersion = 1;
    strcpy_s(ci.applicationInfo.engineName, "GFVR");
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ci.enabledExtensionCount = (uint32_t)exts.size();
    ci.enabledExtensionNames = exts.data();
    XrResult r = xrCreateInstance(&ci, &g_inst);
    if (XR_FAILED(r) && g_gazeExt) {   // never let the optional extension cost VR
        LOGW("xr: xrCreateInstance with eye tracking: %s - retrying without it", Res(r));
        g_gazeExt = false; exts.pop_back(); ci.enabledExtensionCount = (uint32_t)exts.size();
        r = xrCreateInstance(&ci, &g_inst);
    }
    if (XR_FAILED(r)) { LOGW("xr: xrCreateInstance: %s (is SteamVR running?)", Res(r)); g_inst = XR_NULL_HANDLE; return false; }
    if (qpcTime) xrGetInstanceProcAddr(g_inst, "xrConvertWin32PerformanceCounterToTimeKHR", (PFN_xrVoidFunction*)&g_qpcToXr);
    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(g_inst, &ip);
    char b[160];
    snprintf(b, sizeof b, "%s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion),
             XR_VERSION_PATCH(ip.runtimeVersion));
    g_runtime = b;
    LOGI("xr: OpenXR runtime %s (in-process, no XrBridgeHost)", b);
    return true;
}
static bool GetSystem() {
    XrSystemGetInfo gi{XR_TYPE_SYSTEM_GET_INFO};
    gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrResult r = xrGetSystem(g_inst, &gi, &g_sys);
    if (XR_FAILED(r)) { LOGW("xr: xrGetSystem: %s (headset not ready yet?)", Res(r)); return false; }
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    XrSystemEyeGazeInteractionPropertiesEXT gp{XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT};
    if (g_gazeExt) sp.next = &gp;
    xrGetSystemProperties(g_inst, g_sys, &sp);
    g_gazeSys = g_gazeExt && gp.supportsEyeGazeInteraction;
    if (g_gazeExt) LOGI("xr: headset %s eye tracking", g_gazeSys ? "supports" : "does NOT support");
    g_system = sp.systemName;
    uint32_t n = 0;
    xrEnumerateViewConfigurationViews(g_inst, g_sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &n, nullptr);
    std::vector<XrViewConfigurationView> v(n, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(g_inst, g_sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, n, &n, v.data());
    if (n) { g_recW = v[0].recommendedImageRectWidth; g_recH = v[0].recommendedImageRectHeight; }
    LOGI("xr: system '%s', recommended %ux%u per eye, max swapchain %ux%u", sp.systemName, g_recW, g_recH,
         sp.graphicsProperties.maxSwapchainImageWidth, sp.graphicsProperties.maxSwapchainImageHeight);
    return true;
}
static DWORD WINAPI InitThread(LPVOID) {
    for (int i = 0; i < 120 && !g_inst; ++i) { if (CreateInstance()) break; if (g_failed) return 0; Sleep(3000); }
    if (!g_inst) { LOGE("xr: giving up on xrCreateInstance - no VR this run"); return 0; }
    for (int i = 0; !GetSystem(); ++i) { if (i == 200) { LOGE("xr: giving up on xrGetSystem"); return 0; } Sleep(3000); }
    InterlockedExchange(&g_instReady, 1);
    return 0;
}

// ------------------------------------------------------------------ session (render thread)
static bool CreateSession() {
    PFN_xrGetD3D11GraphicsRequirementsKHR getReq = nullptr;
    if (XR_FAILED(xrGetInstanceProcAddr(g_inst, "xrGetD3D11GraphicsRequirementsKHR", (PFN_xrVoidFunction*)&getReq)) || !getReq) {
        LOGE("xr: no xrGetD3D11GraphicsRequirementsKHR"); g_failed = true; return false;
    }
    XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    getReq(g_inst, g_sys, &req);
    IDXGIDevice* dd = nullptr; IDXGIAdapter* ad = nullptr; DXGI_ADAPTER_DESC desc{};
    if (SUCCEEDED(g_dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dd)) && SUCCEEDED(dd->GetAdapter(&ad))) ad->GetDesc(&desc);
    if (ad) ad->Release();
    if (dd) dd->Release();
    bool same = memcmp(&desc.AdapterLuid, &req.adapterLuid, sizeof(LUID)) == 0;
    LOGI("xr: game GPU '%S' %s the headset's GPU; feature level 0x%X (runtime needs 0x%X)", desc.Description, same ? "is" : "is NOT",
         (unsigned)g_dev->GetFeatureLevel(), (unsigned)req.minFeatureLevel);
    XrGraphicsBindingD3D11KHR gb{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    gb.device = g_dev;
    XrSessionCreateInfo si{XR_TYPE_SESSION_CREATE_INFO};
    si.next = &gb;
    si.systemId = g_sys;
    XrResult r = xrCreateSession(g_inst, &si, &g_sess);
    if (XR_FAILED(r)) { LOGE("xr: xrCreateSession: %s - no VR this run", Res(r)); g_failed = true; return false; }
    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.poseInReferenceSpace.orientation.w = 1;
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    xrCreateReferenceSpace(g_sess, &rs, &g_local);
    xrCreateReferenceSpace(g_sess, &rs, &g_ref);
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    xrCreateReferenceSpace(g_sess, &rs, &g_view);
    uint32_t n = 0;
    xrEnumerateSwapchainFormats(g_sess, 0, &n, nullptr);
    g_fmts.resize(n);
    xrEnumerateSwapchainFormats(g_sess, n, &n, g_fmts.data());
    std::string list;
    for (auto f : g_fmts) list += std::to_string(f) + " ";
    LOGI("xr: session created on the game's device; swapchain formats %s", list.c_str());
    if (g_gazeExt) SetupGaze();   // even if the headset says no: some runtimes report it late or wrongly (harmless)
    return true;
}
static void PollEvents() {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(g_inst, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            g_state = ((XrEventDataSessionStateChanged*)&ev)->state;
            LOGI("xr: session state %d", (int)g_state);
            if (g_state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                XrResult r = xrBeginSession(g_sess, &bi);
                g_running = XR_SUCCEEDED(r);
                LOGI("xr: xrBeginSession: %s", Res(r));
            } else if (g_state == XR_SESSION_STATE_STOPPING) {
                if (g_begun) {   // close the open frame first
                    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
                    ei.displayTime = g_fs.predictedDisplayTime;
                    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                    xrEndFrame(g_sess, &ei);
                    g_begun = false;
                }
                xrEndSession(g_sess);
                g_running = false;
                LOGI("xr: session stopped");
            } else if (g_state == XR_SESSION_STATE_EXITING || g_state == XR_SESSION_STATE_LOSS_PENDING) {
                g_running = false; g_failed = true;
                LOGW("xr: session exiting / lost - VR stopped for this run (the game keeps running)");
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            g_running = false; g_failed = true;
            LOGW("xr: instance loss pending - VR stopped");
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

// swapchain format that takes a CopyResource from 'src' (8-bit RGBA / BGRA; sRGB view = the bytes are gamma-encoded)
static int64_t PickFormat(DXGI_FORMAT src) {
    DXGI_FORMAT c[2] = {};
    switch (src) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        c[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; c[1] = DXGI_FORMAT_R8G8B8A8_UNORM; break;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        c[0] = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; c[1] = DXGI_FORMAT_B8G8R8A8_UNORM; break;
    default: return 0;
    }
    for (DXGI_FORMAT f : c) for (auto r : g_fmts) if (r == (int64_t)f) return r;
    return 0;
}
static bool EnsureSwap(Swap& s, ID3D11Texture2D* src, const char* what) {
    D3D11_TEXTURE2D_DESC d{};
    src->GetDesc(&d);
    int64_t want = PickFormat(d.Format);   // same format family (e.g. RGBA8 typeless / UNORM) = same swapchain
    if (s.sc && s.w == d.Width && s.h == d.Height && s.fmt == want) return true;
    if (s.failed && s.src == d.Format && s.w == d.Width && s.h == d.Height) return false;
    if (s.sc) { xrDestroySwapchain(s.sc); s.sc = XR_NULL_HANDLE; s.img.clear(); }
    s.w = d.Width; s.h = d.Height; s.src = d.Format; s.failed = true;
    s.fmt = PickFormat(d.Format);
    if (!s.fmt) { LOGE("xr: no swapchain format takes a copy of %s format %d", what, (int)d.Format); return false; }
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format = s.fmt;
    ci.sampleCount = 1;
    ci.width = d.Width; ci.height = d.Height;
    ci.faceCount = ci.arraySize = ci.mipCount = 1;
    XrResult r = xrCreateSwapchain(g_sess, &ci, &s.sc);
    if (XR_FAILED(r)) { LOGE("xr: %s swapchain %ux%u: %s", what, d.Width, d.Height, Res(r)); s.sc = XR_NULL_HANDLE; return false; }
    uint32_t n = 0;
    xrEnumerateSwapchainImages(s.sc, 0, &n, nullptr);
    s.img.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    xrEnumerateSwapchainImages(s.sc, n, &n, (XrSwapchainImageBaseHeader*)s.img.data());
    s.failed = false;
    LOGI("xr: %s swapchain %ux%u format %lld (source format %d), %u images", what, d.Width, d.Height, (long long)s.fmt, (int)d.Format, n);
    return true;
}
static bool CopyInto(Swap& s, ID3D11Texture2D* src) {
    LARGE_INTEGER a0, a1, af; QueryPerformanceCounter(&a0); QueryPerformanceFrequency(&af);
    struct AcqTime { LARGE_INTEGER& a0; LARGE_INTEGER& a1; LARGE_INTEGER& af; ~AcqTime() { QueryPerformanceCounter(&a1); g_stAcquire += (a1.QuadPart - a0.QuadPart) * 1000.0 / af.QuadPart; } } acq{a0, a1, af};
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = 100000000;   // 100 ms
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    uint32_t ix = 0;
    if (XR_FAILED(xrAcquireSwapchainImage(s.sc, &ai, &ix))) return false;
    bool ok = XR_SUCCEEDED(xrWaitSwapchainImage(s.sc, &wi));
    if (ok) g_ctx->CopySubresourceRegion(s.img[ix].texture, 0, 0, 0, 0, src, 0, nullptr);
    if (ok && &s == &g_eye[0]) fvr::ProbeTex(2, "OpenXR left swapchain image", g_dev, g_ctx, s.img[ix].texture);
    xrReleaseSwapchainImage(s.sc, &ri);
    return ok;
}

static float Yaw(const XrQuaternionf& q) {
    float fx = -2.0f * (q.x * q.z + q.w * q.y);
    float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    return std::atan2(-fx, -fz);
}
static void Recenter(XrTime t) {
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(xrLocateSpace(g_view, g_local, t, &loc))) return;
    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if ((loc.locationFlags & need) != need) return;
    InterlockedExchange(&g_recenterReq, 0);
    float yaw = Yaw(loc.pose.orientation);
    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rs.poseInReferenceSpace.orientation = {0, std::sin(yaw * 0.5f), 0, std::cos(yaw * 0.5f)};
    rs.poseInReferenceSpace.position = loc.pose.position;
    XrSpace ns = XR_NULL_HANDLE;
    if (XR_FAILED(xrCreateReferenceSpace(g_sess, &rs, &ns))) return;
    if (g_ref) xrDestroySpace(g_ref);
    g_ref = ns;
    LOGI("xr: recentred (yaw %.1f deg, position %.2f %.2f %.2f)", yaw * 57.2958f, loc.pose.position.x, loc.pose.position.y, loc.pose.position.z);
}

// ------------------------------------------------------------------ per frame (game's Present thread)
// xrWaitFrame + xrBeginFrame + the head pose for this frame's display time.
// Start-of-frame pacing (XrBeginFrame11 right after the game's Present): the game then renders the frame with the pose
// predicted for the time it is displayed, and the headset paces the game here. Without it (old order), wait / begin /
// end all ran at Present after the frame was already rendered: every frame reached the compositor with a display time
// that its GPU work could not meet (user: 56 fps rendered, a slideshow in the headset) and poses were a frame old.
static void BeginXrFrame() {
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    XrFrameState& fs = g_fs;
    fs = {XR_TYPE_FRAME_STATE};
    XrResult r = xrWaitFrame(g_sess, nullptr, &fs);
    QueryPerformanceCounter(&t1);
    if (XR_FAILED(r)) { static int n = 0; if (n++ < 5) LOGE("xr: xrWaitFrame: %s", Res(r)); return; }
    g_statWait += (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
    ++g_statXr;
    ++g_frames;
    g_period = fs.predictedDisplayPeriod;
    if (XrTime now = XrNow()) {
        if (g_lastBegin) { double dt = (now - g_lastBegin) / 1e6; if (dt > 0 && dt < 500) g_frameEma = g_frameEma ? g_frameEma * 0.9 + dt * 0.1 : dt; }
        g_lastBegin = now;
        XrTime target = now + (XrTime)((g_frameEma + fs.predictedDisplayPeriod / 1e6) * 1e6);
        if (g_fixLateTime && fs.predictedDisplayTime < now + fs.predictedDisplayPeriod / 2) {
            if (g_prevDisplay && target <= g_prevDisplay) target = g_prevDisplay + fs.predictedDisplayPeriod;   // always forward
            fs.predictedDisplayTime = target; g_stFixed++;
        }
        g_stLeadWait += (fs.predictedDisplayTime - now) / 1e6;
        if (g_prevDisplay) {
            double step = (fs.predictedDisplayTime - g_prevDisplay) / 1e6;
            g_stStep += step;
            if (step <= 0) g_stSame++;
        }
        g_stPeriod += fs.predictedDisplayPeriod / 1e6;
        g_stN++;
    }
    g_prevDisplay = fs.predictedDisplayTime;
    if (!fs.shouldRender) g_stNoRender++;
    r = xrBeginFrame(g_sess, nullptr);
    if (XR_FAILED(r)) { static int n = 0; if (n++ < 5) LOGE("xr: xrBeginFrame: %s", Res(r)); return; }
    g_begun = true;

    if (g_recenterReq) Recenter(fs.predictedDisplayTime);
    // poses for the next rendered frame (ClientGetLatestPoses)
    {
        XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
        li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        li.displayTime = fs.predictedDisplayTime;
        li.space = g_ref;
        XrViewState vs{XR_TYPE_VIEW_STATE};
        XrView xv[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        uint32_t nv = 0;
        if (XR_SUCCEEDED(xrLocateViews(g_sess, &li, &vs, 2, &nv, xv)) && nv == 2 &&
            (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
            for (int e = 0; e < 2; ++e) {
                g_pose[e] = {xv[e].pose.position.x, xv[e].pose.position.y, xv[e].pose.position.z,
                             xv[e].pose.orientation.x, xv[e].pose.orientation.y, xv[e].pose.orientation.z, xv[e].pose.orientation.w};
                g_fov[e] = {xv[e].fov.angleLeft, xv[e].fov.angleRight, xv[e].fov.angleUp, xv[e].fov.angleDown};
            }
            SimulateHead(g_pose, fs.predictedDisplayTime);
            g_poseTime = fs.predictedDisplayTime;
            UpdateGaze(fs.predictedDisplayTime);
            if (!g_poseOk) LOGI("xr: tracking - eye fov L %.1f R %.1f U %.1f D %.1f deg, eye distance %.1f mm", g_fov[0].left * 57.2958f,
                                g_fov[0].right * 57.2958f, g_fov[0].up * 57.2958f, g_fov[0].down * 57.2958f,
                                1000.f * std::sqrt((g_pose[0].px - g_pose[1].px) * (g_pose[0].px - g_pose[1].px) +
                                                   (g_pose[0].py - g_pose[1].py) * (g_pose[0].py - g_pose[1].py) +
                                                   (g_pose[0].pz - g_pose[1].pz) * (g_pose[0].pz - g_pose[1].pz)));
            g_poseOk = true;
        }
    }
}

void XrBeginFrame11(bool enabled) {
    static int last = -1;
    if (last != (int)enabled) { if (last >= 0) LOGI("xr: frame start pacing %s", enabled ? "ON (frame starts after Present)" : "OFF (old order: all at Present)"); last = enabled; }
    if (!enabled || g_failed || !g_instReady || !g_sess) return;
    PollEvents();
    if (!g_running || g_begun) return;
    BeginXrFrame();
}

void XrFrame11(ID3D11Device* dev, ID3D11DeviceContext* ctx, const Submit11& s) {
    ++g_statPresents;
    DWORD now = GetTickCount();
    if (now - g_statTick >= 10000) {
        if (g_statTick && g_statXr)
            LOGI("xr: %.1f game frames/s, %.1f OpenXR frames/s, xrWaitFrame %.1f ms avg; layers: %llu stereo, %llu HUD, %llu flat; state %d",
                 g_statPresents * 1000.0 / (now - g_statTick), g_statXr * 1000.0 / (now - g_statTick), g_statWait / g_statXr,
                 g_statLayers[0], g_statLayers[1], g_statLayers[2], (int)g_state);
        if (g_stN && g_stEndN && g_statXr)
            LOGI("xr timing: display time at xrWaitFrame %.1f ms ahead, at xrEndFrame %.1f ms ahead (%llu of %llu frames LATE = display time already past), "
                 "display time step %.1f ms per frame (period %.1f ms; %llu repeated), shouldRender false %llu; %llu display times replaced (late fix %s); swapchain copies %.1f ms, xrEndFrame %.1f ms per frame",
                 g_stLeadWait / g_stN, g_stLeadEnd / g_stEndN, g_stLate, g_stEndN, g_stN > 1 ? g_stStep / (g_stN - 1) : 0.0, g_stPeriod / g_stN, g_stSame,
                 g_stNoRender, g_stFixed, g_fixLateTime ? "on" : "off", g_stAcquire / g_statXr, g_stEnd / g_statXr);
        { std::string pl = fvr::ProbeLine(); if (!pl.empty()) LOGI("%s", pl.c_str()); }
        if (!g_qpcToXr && g_statXr) LOGI("xr timing: runtime has no %s", XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME);
        g_stLeadWait = g_stLeadEnd = g_stStep = g_stPeriod = g_stAcquire = g_stEnd = 0; g_stN = g_stLate = g_stSame = g_stNoRender = g_stEndN = g_stFixed = 0;
        g_statPresents = g_statXr = 0; g_statWait = 0; memset(g_statLayers, 0, sizeof g_statLayers);
        g_statTick = now;
    }
    if (g_failed || !g_instReady || !dev || !ctx) return;
    if (!g_dev) { g_dev = dev; g_ctx = ctx; }
    if (dev != g_dev) return;   // the game made a new device: OpenXR stays on the first one
    if (!g_sess && !CreateSession()) return;
    PollEvents();
    if (!g_running) return;

    if (!g_begun) BeginXrFrame();
    if (!g_begun) return;
    XrFrameState& fs = g_fs;
    XrResult r;

    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    const XrCompositionLayerBaseHeader* layers[2];
    uint32_t nl = 0;
    if (fs.shouldRender) {
        bool stereo = s.stereo && s.eye[0] && s.eye[1];
        if (stereo) {
            for (int e = 0; e < 2 && stereo; ++e) {
                stereo = EnsureSwap(g_eye[e], s.eye[e], e ? "right eye" : "left eye") && CopyInto(g_eye[e], s.eye[e]);
                const IpcPose& p = s.pose[e];
                pv[e].pose.orientation = {p.qx, p.qy, p.qz, p.qw};
                pv[e].pose.position = {p.px, p.py, p.pz};
                pv[e].fov = {s.fov[e].left, s.fov[e].right, s.fov[e].up, s.fov[e].down};
                pv[e].subImage.swapchain = g_eye[e].sc;
                pv[e].subImage.imageRect = {{0, 0}, {(int32_t)g_eye[e].w, (int32_t)g_eye[e].h}};
                pv[e].subImage.imageArrayIndex = 0;
            }
            if (stereo) {
                proj.space = g_ref;
                proj.viewCount = 2;
                proj.views = pv;
                layers[nl++] = (XrCompositionLayerBaseHeader*)&proj;
                g_statLayers[0]++;
            }
        }
        // panel: the HUD over the stereo view, or (no stereo frame) the game image on a flat screen
        bool hud = stereo && s.panelIsHud;
        if (s.panel && (hud || !stereo) && EnsureSwap(g_panel, s.panel, "panel") && CopyInto(g_panel, s.panel)) {
            float dist = hud ? g_cs.hudDistance : g_cs.menuDistance, w = hud ? g_cs.hudWidth : g_cs.menuWidth;
            quad.layerFlags = hud ? (XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                     (g_cs.hudPremultiplied ? 0 : XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT))
                                  : 0;   // flat screen: opaque
            quad.space = g_ref;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = g_panel.sc;
            quad.subImage.imageRect = {{0, 0}, {(int32_t)g_panel.w, (int32_t)g_panel.h}};
            quad.pose.orientation = {0, 0, 0, 1};
            quad.pose.position = {0, hud ? g_cs.hudOffsetY : 0.f, -dist};
            quad.size = {w, w * (float)g_panel.h / (float)g_panel.w};
            layers[nl++] = (XrCompositionLayerBaseHeader*)&quad;
            g_statLayers[hud ? 1 : 2]++;
        }
    }
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = nl;
    ei.layers = layers;
    LARGE_INTEGER e0, e1, ef; QueryPerformanceCounter(&e0); QueryPerformanceFrequency(&ef);
    if (XrTime now = XrNow()) {
        double lead = (ei.displayTime - now) / 1e6;
        g_stLeadEnd += lead;
        if (lead < 0) g_stLate++;
        g_stEndN++;
    }
    r = xrEndFrame(g_sess, &ei);
    QueryPerformanceCounter(&e1);
    g_stEnd += (e1.QuadPart - e0.QuadPart) * 1000.0 / ef.QuadPart;
    g_begun = false;
    if (XR_FAILED(r)) { static int n = 0; if (n++ < 10) LOGE("xr: xrEndFrame: %s", Res(r)); }
}

// ------------------------------------------------------------------ the xrb client API
bool ClientInit(const ClientSettings& s) {
    if (InterlockedExchange(&g_initStarted, 1)) return true;
    g_cs = s;
    LOGI("xr: in-process OpenXR (no XrBridgeHost.exe) - waiting for the runtime");
    if (HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr)) CloseHandle(t);
    return true;
}
void ClientShutdown() {}   // the runtime cleans up with the process (no calls from DllMain at exit)
bool ClientHostConnected() { return g_running && g_poseOk && !g_failed; }
bool ClientHostVisible() { return ClientHostConnected() && (g_state == XR_SESSION_STATE_VISIBLE || g_state == XR_SESSION_STATE_FOCUSED); }
const char* ClientHostError() { return g_failed ? "OpenXR failed - see the log" : ""; }
bool ClientWaitRecommendedSize(unsigned* w, unsigned* h, unsigned timeoutMs) {
    DWORD t0 = GetTickCount();
    while (!g_instReady && GetTickCount() - t0 < timeoutMs && !g_failed) Sleep(50);
    if (!g_instReady) return false;
    *w = g_recW; *h = g_recH;
    return g_recW && g_recH;
}
uint64_t ClientAdapterLuid() { return 0; }
bool ClientGetLatestPoses(IpcPose pose[2], IpcFov fov[2], int64_t* time) {
    if (!ClientHostConnected()) return false;
    memcpy(pose, g_pose, sizeof g_pose);
    memcpy(fov, g_fov, sizeof g_fov);
    *time = g_poseTime;
    return true;
}
void ClientPublishTextures(const TextureSet&) {}
int ClientAcquireSlot() { return -1; }
void ClientCancelSlot(int) {}
void ClientSubmitSlot(int, const IpcPose[2], const IpcFov[2], int64_t, uint32_t, const IpcMotion*) {}
int ClientLastSubmittedSlot() { return -1; }
void ClientRequestRecenter() { InterlockedExchange(&g_recenterReq, 1); }
void ClientRequestDump(uint32_t) {}
bool ClientWaitHostFrame(unsigned) { return false; }
uint32_t ClientHostFrameCount() { return g_frames; }
int64_t ClientDisplayPeriodNs() { return g_period; }
void ClientSetHud(float distance, float width, float offsetY, bool followHead) {
    g_cs.hudDistance = distance; g_cs.hudWidth = width; g_cs.hudOffsetY = offsetY; g_cs.hudFollowHead = followHead;
}
void ClientSetMenu(float distance, float width) { g_cs.menuDistance = distance; g_cs.menuWidth = width; }

void PoseToViewRH(const IpcPose& p, float o[16]) {
    float x = p.qx, y = p.qy, z = p.qz, w = p.qw;
    float r[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                     {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                     {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) o[i * 4 + j] = r[i][j];
    o[3] = o[7] = o[11] = 0;
    for (int j = 0; j < 3; ++j) o[12 + j] = -(p.px * r[0][j] + p.py * r[1][j] + p.pz * r[2][j]);
    o[15] = 1;
}
void FovToProjRH(const IpcFov& f, float n, float fa, float o[16]) {
    float l = tanf(f.left), r = tanf(f.right), u = tanf(f.up), d = tanf(f.down);
    float w = r - l, h = u - d;
    memset(o, 0, sizeof(float) * 16);
    o[0] = 2 / w; o[5] = 2 / h; o[8] = (r + l) / w; o[9] = (u + d) / h;
    o[10] = fa / (n - fa); o[11] = -1; o[14] = n * fa / (n - fa);
}

} // namespace xrb
