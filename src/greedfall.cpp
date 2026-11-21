// GreedFall-specific fixes (Spiders' Silk engine). Addresses are RVAs in the Steam build's Engine.dll / Game.dll
// (2026-11 install, see FINDINGS.md); each hook checks the function's first bytes first and stays off on a mismatch.
//   - Dynamic resolution: Engine.dll graphics-settings interface slot 24 = get "RENDER\DynamicRes" -> forced false.
//   - Head bob: Game.dll camera update 0x4339F0 applies bob offset (+0x7F0, vec4), roll (+0x8DC), pitch (+0x9BC);
//     they are zeroed for the call and restored after it (the game's bob simulation keeps its state).
//   - Camera shake: Game.dll debug switch ACTIVATION\DisableAllCameraShake = byte at 0xE54890.
#include "greedfall.h"
#include "d3d11hooks.h"
#include "stereo.h"
#include "xrbridge/client.h"
#include "globals.h"
#include "log.h"
#include "MinHook.h"
#include <cstdint>
namespace fvr { void CamWatchStart(uint8_t* node, int ms); }
#include <windows.h>
#include <string>
#include <intrin.h>
#include <cmath>
#include <cstring>
#include <cmath>

namespace fvr {
bool D3d11MainCameraPos(float out[3]);
namespace {

HMODULE g_engine = nullptr, g_game = nullptr;
volatile uint8_t* g_shakeOff = nullptr;
volatile long g_bobCalls = 0, g_dynResCalls = 0;

bool BytesMatch(HMODULE m, uint32_t rva, const char* hex) {
    const uint8_t* p = (const uint8_t*)m + rva;
    for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) {
        char b[3] = {hex[i], hex[i + 1], 0};
        if (p[i / 2] != (uint8_t)strtoul(b, nullptr, 16)) return false;
    }
    return true;
}

bool Hook(void* target, void* fn, void** orig, const char* what) {
    static bool init = [] { MH_STATUS s = MH_Initialize(); return s == MH_OK || s == MH_ERROR_ALREADY_INITIALIZED; }();
    if (!init) return false;
    MH_STATUS s = MH_CreateHook(target, fn, orig);
    if (s == MH_OK) s = MH_EnableHook(target);
    if (s != MH_OK) { LOGE("greedfall: hook %s at %p failed (%d)", what, target, (int)s); return false; }
    LOGI("greedfall: hooked %s at %p", what, target);
    return true;
}

// ---- dynamic resolution
typedef bool(__fastcall* GetBool_t)(void* self);
GetBool_t o_GetDynamicRes = nullptr;
bool __fastcall Hk_GetDynamicRes(void* self) {
    bool game = o_GetDynamicRes(self);
    if (InterlockedIncrement(&g_dynResCalls) <= 3) LOGI("greedfall: game asked for dynamic resolution (setting %d) -> %s", game,
                                                        G().s.gfNoDynamicRes ? "off (forced)" : "unchanged");
    return G().s.gfNoDynamicRes ? false : game;
}

// ---- head bob
typedef void(__fastcall* CamUpdate_t)(uint8_t* self, float dt);
CamUpdate_t o_CamUpdate = nullptr;
void __fastcall Hk_CamUpdate(uint8_t* self, float dt) {
    if (!G().s.gfNoHeadBob || !self) { o_CamUpdate(self, dt); return; }
    alignas(16) uint8_t off[16];
    float roll, pitch;
    memcpy(off, self + 0x7F0, 16); memcpy(&roll, self + 0x8DC, 4); memcpy(&pitch, self + 0x9BC, 4);
    memset(self + 0x7F0, 0, 16); memset(self + 0x8DC, 0, 4); memset(self + 0x9BC, 0, 4);
    o_CamUpdate(self, dt);
    memcpy(self + 0x7F0, off, 16); memcpy(self + 0x8DC, &roll, 4); memcpy(self + 0x9BC, &pitch, 4);
    long n = InterlockedIncrement(&g_bobCalls);
    if (n == 1 || n == 600)
        LOGI("greedfall: head bob removed (call %ld: offset %.3f %.3f %.3f, roll %.3f, pitch %.3f)", n, ((float*)off)[0], ((float*)off)[1],
             ((float*)off)[2], roll, pitch);
}

// ---- culling: the game culls with its own camera FOV (~72 x 44 deg); the headset shows ~100+ deg. PCamera::setFOV
//      (Engine.dll 0x568530; PCameraNode::setFOV 0x568550 = camera at +0x1E0) gets a wider angle: tan(fov/2) * cull_fov.
//      The eye projections are the headset's own, so only culling / LOD see the wider angle.
typedef void(__fastcall* SetFov_t)(uint8_t* cam, float fov);
SetFov_t o_SetFov = nullptr, o_SetFovNode = nullptr;
struct FovSeen { void* cam; float in, out; };
FovSeen g_fovSeen[16];
SRWLOCK g_fovLock = SRWLOCK_INIT;
// Head-following culling: the game culls with its own camera, which does not turn with the headset. Looking 40 deg down,
// the ground in front of the character fell outside that frustum and was not drawn (the ocean showed through; test 033).
// The widening factor grows with the head's angle away from the game camera so the culled frustum covers the headset's
// view: needed half angle = head offset + the headset's half FOV (+ margin), capped at 79 deg (158 deg total).
static volatile LONG g_setFovCalls = 0;
static float g_lastHeadK = 1;
static float HeadCullFactor(float gameHalfV) {
    xrb::IpcPose pose[2]; xrb::IpcFov fov[2]; int64_t t;
    if (!xrb::ClientGetLatestPoses(pose, fov, &t)) return 1;
    // head forward = -Z rotated by the (left) eye orientation, in the recentred space (= game camera direction)
    float qx = pose[0].qx, qy = pose[0].qy, qz = pose[0].qz, qw = pose[0].qw;
    float fx = -2.f * (qx * qz + qw * qy), fy = -2.f * (qy * qz - qw * qx), fz = -(1.f - 2.f * (qx * qx + qy * qy));
    float pitch = fabsf(asinf(fy < -1 ? -1 : fy > 1 ? 1 : fy));
    float yaw = fabsf(atan2f(-fx, -fz));
    float hmdV = (fabsf(fov[0].up) > fabsf(fov[0].down) ? fabsf(fov[0].up) : fabsf(fov[0].down));
    float hmdH = (fabsf(fov[0].left) > fabsf(fov[0].right) ? fabsf(fov[0].left) : fabsf(fov[0].right));
    const float margin = 0.09f, cap = 1.379f;   // 5 deg, 79 deg
    float needV = pitch + hmdV + margin, needH = yaw + hmdH + margin;
    if (needV > cap) needV = cap;
    if (needH > cap) needH = cap;
    float gameHalfH = atanf(tanf(gameHalfV) * 16.f / 9.f);
    float kv = tanf(needV) / tanf(gameHalfV), kh = tanf(needH) / tanf(gameHalfH);
    return kv > kh ? kv : kh;
}
static bool g_camCopyOk = false;
static float g_lastK = 0;
float Widen(void* cam, float fov) {
    InterlockedIncrement(&g_setFovCalls);
    float k = G().s.cullFov;
    if (G().s.cullFollowHead) {
        float half = (fov < 3.2f ? fov : fov / 57.2958f) * 0.5f;
        float hk = HeadCullFactor(half);
        g_lastHeadK = hk;
        if (hk > k) k = hk;
    }
    // Game camera follows the head (snapshot turned): the culling view only has to cover the headset's own FOV (+ margin
    // for the pose used one frame earlier), so it is sized from the headset - far less widening, so the engine's
    // "too small to draw" / LOD checks see near-real sizes (sharedtools\culling.txt: big widening = pop-in ahead).
    if (G().s.engineHead && g_camCopyOk) {
        float vMax, hMax;
        if (StereoHeadsetTans(&vMax, &hMax)) {
            float half0 = (fov < 3.2f ? fov : fov / 57.2958f) * 0.5f, t0 = tanf(half0);
            float asp = *(float*)((uint8_t*)cam + 0x14); if (!(asp > 0.5f && asp < 4.f)) asp = 1.7778f;
            float need = fmaxf(vMax, hMax / asp) * G().s.cullMargin;
            k = t0 > 0.01f ? fmaxf(1.f, need / t0) : 1.f;
            g_lastK = k;
        }
    }
    // units: radians (< 3.2) or degrees; only ordinary perspective cameras (20..85 deg) - not cubemap / reflection captures.
    // Cinematics use narrow cameras (5..20 deg, user log d748cf5d: "main camera FOV 19.4 x 11.0"): with cine_cull_widen
    // these are widened too (down to 1 deg), else the head-turned culling view is only ~11 deg tall -> heads / floor vanish.
    bool rad = fov < 3.2f;
    float deg = rad ? fov * 57.2958f : fov;
    AcquireSRWLockExclusive(&g_fovLock);
    float out = fov;
    int slot = -1;
    for (int i = 0; i < 16; ++i) if (g_fovSeen[i].cam == cam) { slot = i; break; }
    if (slot >= 0 && fov == g_fovSeen[slot].out) out = fov;   // our own value coming back: don't widen twice
    else {
        float minDeg = G().s.cineCull ? 1.f : 20.f;
        if (k > 1.001f && deg > minDeg && deg < 85.f) {
            float half = (rad ? fov : fov / 57.2958f) * 0.5f;
            float w = 2.f * atanf(tanf(half) * k);
            if (w > 2.76f) w = 2.76f;   // <= 158 deg
            out = rad ? w : w * 57.2958f;
        }
        if (slot < 0) for (int i = 0; i < 16; ++i) if (!g_fovSeen[i].cam) { slot = i; break; }
        if (slot >= 0) {
            static int logged = 0;
            if (g_fovSeen[slot].cam != cam && logged++ < 12)
                LOGI("greedfall: camera %p FOV %.4f (%s) -> %.4f for culling (cull_fov %.2f)", cam, fov, rad ? "rad" : "deg", out, k);
            g_fovSeen[slot] = {cam, fov, out};
        }
    }
    ReleaseSRWLockExclusive(&g_fovLock);
    return out;
}
// Camera RE aid (camera_dump = 1): once in gameplay, dump the memory of the render camera (the per-frame setFOV call with an
// ordinary FOV) around the camera / node pointer, next to the camera position the shaders received, to locate its matrix.
static void DumpCamera(const char* via, uint8_t* p, float fov, void* ret) {
    static int dumps = 0;
    if (!G().s.cameraDump || dumps >= 3 || G().frame < 600) return;
    float deg = fov < 3.2f ? fov * 57.2958f : fov;
    if (deg < 20 || deg > 85) return;
    float cp[3];
    if (!D3d11MainCameraPos(cp)) return;
    static long lastFrame = -100;
    if (G().frame - lastFrame < 200) return;
    lastFrame = G().frame;
    dumps++;
    LOGI("camdump %d: via %s ptr %p fov %.3f caller %p (Engine.dll+%llx) frame %ld; shader camera pos %.3f %.3f %.3f", dumps, via, (void*)p, fov, ret,
         (unsigned long long)((uint8_t*)ret - (uint8_t*)g_engine), G().frame, cp[0], cp[1], cp[2]);
    uint8_t* base = p - 0x200;
    for (int off = 0; off < 0x600; off += 16) {
        float* f = (float*)(base + off);
        if (IsBadReadPtr(f, 16)) continue;
        bool hit = false;
        for (int i = 0; i < 4; ++i) for (int k = 0; k < 3; ++k) if (fabsf(f[i] - cp[k]) < 0.05f + 0.001f * fabsf(cp[k])) hit = true;
        LOGI("camdump %d: %+05x  %12.5f %12.5f %12.5f %12.5f%s", dumps, off - 0x200, f[0], f[1], f[2], f[3], hit ? "   <- camera pos value" : "");
    }
}
void __fastcall Hk_SetFov(uint8_t* cam, float fov) {
    void* ret = _ReturnAddress();
    {
        static uint32_t seen[16]; static int n = 0;
        uint32_t caller = (uint32_t)((uint8_t*)ret - (uint8_t*)g_engine);
        bool known = false;
        for (int i = 0; i < n; ++i) if (seen[i] == caller) known = true;
        if (!known && n < 16) { seen[n++] = caller; LOGI("greedfall: PCamera::setFOV call site Engine.dll+%x cam %p fov %.2f frame %ld", caller, (void*)cam, fov, G().frame); }
    }
    DumpCamera("PCamera::setFOV (cam)", cam, fov, ret);
    o_SetFov(cam, Widen(cam, fov));
}
// ---- engine head: turn the game's render camera with the head. PCameraNode (found with camera_dump, test 035):
//   +0xB0 local matrix, +0xF0 world matrix (row vectors: X, Y, Z axes, position; looks along -Z like OpenXR),
//   +0x160 view = inverse(world), +0x1A0 view * projection, +0x1E0 PCamera (+0x200 projection matrix).
// The render camera's setFOV runs every frame from Engine.dll+0x7126BD while the frame is set up; there the world / view /
// view-projection get the head: world' = H * world. If the engine did not rewrite the world matrix since our last write,
// the stored unrotated one is used (never stacks the rotation).
static const uint32_t kRenderCamCaller = 0x7126BD;        // title / menu scenes
static const uint32_t kRenderCamCallerGame = 0x34DA98;   // gameplay (test 037)
struct CamHead { uint8_t* node = nullptr; float base[16], written[16]; bool have = false; } g_camHead;
static volatile LONG g_camHeadApplied = 0;
static void Mul44(const float* a, const float* b, float* o) {
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) {
        float s = 0; for (int k = 0; k < 4; ++k) s += a[i * 4 + k] * b[k * 4 + j];
        o[i * 4 + j] = s;
    }
}
static void InvRigid(const float* m, float* o) {   // inverse of a rotation + translation (row vectors)
    for (int i = 0; i < 3; ++i) { for (int j = 0; j < 3; ++j) o[i * 4 + j] = m[j * 4 + i]; o[i * 4 + 3] = 0; }
    for (int j = 0; j < 3; ++j) o[12 + j] = -(m[12] * o[0 * 4 + j] + m[13] * o[1 * 4 + j] + m[14] * o[2 * 4 + j]);
    o[15] = 1;
}
// ---- head-aimed culling (sharedtools\culling.txt). Camera read watch (test 063): Engine.dll+0x7A720 updates the
// render camera's view*proj (+0x1A0) through the 4x4 multiply at +0x7A150 (out = B * A; rcx = A, rdx = out, r8 = B),
// and the same multiply is called with A = the render camera's view*proj from the scene code (2x per frame).
// There A is swapped for view * E_rot * proj (E_rot = head rotation as used for the eyes), so that use follows the head
// while the camera itself, rendering and picking keep the game's values.
static uint8_t* volatile g_cullNode = nullptr;
static void(__fastcall* o_MatMul)(const float* A, float* out, const float* B) = nullptr;
static volatile LONG g_cullAimCalls = 0;
static void* g_cullAimRet[8]; static volatile LONG g_cullAimRetN = 0;
static void __fastcall Hk_MatMul(const float* A, float* out, const float* B) {
    uint8_t* n = g_cullNode;
    if (n && A == (const float*)(n + 0x1A0)) {
        void* ret = _ReturnAddress();
        LONG k = g_cullAimRetN; bool known = false;
        for (LONG i = 0; i < k; ++i) known |= g_cullAimRet[i] == ret;
        if (!known && k < 8) { g_cullAimRet[k] = ret; g_cullAimRetN = k + 1; LOGI("greedfall: render camera view*proj multiplied from Engine.dll+%llx", (unsigned long long)((uint8_t*)ret - (uint8_t*)g_engine)); }
        if (G().s.cullAim || G().s.cullAimTestYaw != 0 || G().s.cullAimTestPitch != 0) {
            float E[16];
            if (StereoCullRotation11(E, G().s.cullAim, G().s.cullAimTestYaw, G().s.cullAimTestPitch)) {
                alignas(16) float Vc[16], VPc[16];
                Mul44((const float*)(n + 0x160), E, Vc);
                Mul44(Vc, (const float*)(n + 0x200), VPc);
                InterlockedIncrement(&g_cullAimCalls);
                o_MatMul(VPc, out, B);
                return;
            }
        }
    }
    o_MatMul(A, out, B);
}
// The engine is built with whole-program optimisation: callers of the multiply assume that registers it does not touch
// survive (test 064 crashed at frame 1 with a plain C++ detour). The stub keeps rcx, rdx, r8-r11 and returns rax = out
// like the original (xmm0-5 are clobbered by the original too; xmm6-15 are kept by the C++ ABI).
static void* MakeStub(void* fn, bool retRcx);
static void* MakeMatMulStub() { return MakeStub((void*)&Hk_MatMul, false); }
static void* MakeStub(void* fnPtr, bool retRcx) {
    uint8_t* p = (uint8_t*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!p) return nullptr;
    uint8_t code[] = {0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53,   // push rcx rdx r8 r9 r10 r11
                      0x48, 0x83, 0xEC, 0x28,                                         // sub rsp, 28h (aligned for the call)
                      0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,                             // mov rax, Hk_MatMul
                      0xFF, 0xD0,                                                     // call rax
                      0x48, 0x83, 0xC4, 0x28,                                         // add rsp, 28h
                      0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59,   // pop r11 r10 r9 r8 rdx rcx
                      0x48, 0x8B, 0xC2, 0xC3};                                        // mov rax, rdx; ret
    void* fn = fnPtr; memcpy(code + 16, &fn, 8);
    if (retRcx) code[sizeof code - 2] = 0xC1;   // mov rax, rcx
    memcpy(p, code, sizeof code);
    FlushInstructionCache(GetCurrentProcess(), p, sizeof code);
    return p;
}
// Camera snapshot (test 066 RE): Engine.dll+0x567270 (rcx = destination, rdx = source camera node) builds a PCameraNode
// copy on the stack for the frame: +0x60 world, +0xA0 view, +0xE0 view*proj (from node +0xF0 / +0x160 / +0x1A0).
// Experiment: turn only that copy (cull_aim_copy) - does culling follow?
static void(__fastcall* o_CamCopy)(uint8_t* dst, const uint8_t* src) = nullptr;
static volatile LONG g_camCopyCalls = 0, g_camCopyRender = 0;
static void __fastcall Hk_CamCopy(uint8_t* dst, const uint8_t* src) {
    o_CamCopy(dst, src);
    InterlockedIncrement(&g_camCopyCalls);
    uint8_t* n = g_cullNode;
    if (!n || src != n) return;
    InterlockedIncrement(&g_camCopyRender);
    bool test = G().s.cullAimCopy && (G().s.cullAimTestYaw != 0 || G().s.cullAimTestPitch != 0);
    if (!G().s.engineHead && !test) return;
    float Edec[16], Enode[16];
    if (!StereoCullRotation11(Edec, G().s.engineHead, G().s.cullAimTestYaw, G().s.cullAimTestPitch)) return;
    bool flip = StereoCullFlip((const float*)(src + 0x1A0), (const float*)(src + 0x160));
    StereoCullRotToNode(Edec, flip, Enode);
    // view' = view * R (as for the eyes); world' = inverse(view'); vp' = view' * proj
    alignas(16) float v2[16], w2[16], vp2[16];
    Mul44((const float*)(src + 0x160), Enode, v2);
    InvRigid(v2, w2);
    Mul44(v2, (const float*)(src + 0x200), vp2);
    memcpy(dst + 0x60, w2, 64); memcpy(dst + 0xA0, v2, 64); memcpy(dst + 0xE0, vp2, 64);
    if (G().s.engineHead) StereoRecordCullCopy(v2, Edec, flip);
    static bool logged = false;
    if (!logged) { logged = true; LOGI("greedfall: game camera follows the head via the frame's camera snapshot (view z %s)", flip ? "flipped vs. decomposition" : "same as decomposition"); }
}
long GfCamCopyStats(long* render) { *render = InterlockedExchange(&g_camCopyRender, 0); return InterlockedExchange(&g_camCopyCalls, 0); }
long GfCullAimCalls() { return InterlockedExchange(&g_cullAimCalls, 0); }
static void ApplyEngineHead(uint8_t* node, void* ret) {
    uint32_t caller = (uint32_t)((uint8_t*)ret - (uint8_t*)g_engine);
    if (caller == kRenderCamCaller || caller == kRenderCamCallerGame || caller == G().s.gfRenderCamCaller) g_cullNode = node;
    if (G().s.camWatchMs > 0 && caller == kRenderCamCallerGame) { static int n = 0; if (++n == 1500) ::fvr::CamWatchStart(node, G().s.camWatchMs); }   // test aid
    return;   // retired (build 14): rewriting the node broke parts of the engine; the frame snapshot is turned instead
    {   // log the call sites (RE: which one is the render camera in gameplay)
        static uint32_t seen[16]; static int n = 0;
        bool known = false;
        for (int i = 0; i < n; ++i) if (seen[i] == caller) known = true;
        if (!known && n < 16) {
            seen[n++] = caller;
            float* w = (float*)(node + 0xF0);
            LOGI("greedfall: setFOV call site Engine.dll+%x node %p fov %.2f frame %ld cam pos %.1f %.1f %.1f", caller, (void*)node,
                 *(float*)(node + 0x1F0), G().frame, w[12], w[13], w[14]);
        }
    }
    float deg = *(float*)(node + 0x1E0 + 0x10);
    (void)deg;
    if (caller != kRenderCamCaller && caller != kRenderCamCallerGame && caller != G().s.gfRenderCamCaller) return;
    float H[16];
    if (!StereoEngineHead(H)) return;
    float* world = (float*)(node + 0xF0);
    float* view = (float*)(node + 0x160);
    float* vp = (float*)(node + 0x1A0);
    const float* proj = (const float*)(node + 0x200);
    if (g_camHead.node != node) { g_camHead = CamHead(); g_camHead.node = node; }
    float base[16];
    if (g_camHead.have && memcmp(world, g_camHead.written, 64) == 0) memcpy(base, g_camHead.base, 64);   // not rewritten: undo ours
    else memcpy(base, world, 64);
    float w2[16], v2[16], vp2[16];
    Mul44(H, base, w2);
    InvRigid(w2, v2);
    Mul44(v2, proj, vp2);
    memcpy(world, w2, 64); memcpy(view, v2, 64); memcpy(vp, vp2, 64);
    memcpy(g_camHead.base, base, 64); memcpy(g_camHead.written, w2, 64); g_camHead.have = true;
    StereoEngineHeadApplied();
    if (InterlockedIncrement(&g_camHeadApplied) == 1)
        LOGI("greedfall: engine head - render camera %p turned with the head (camera pos %.1f %.1f %.1f)", (void*)node, w2[12], w2[13], w2[14]);
}
void __fastcall Hk_SetFovNode(uint8_t* node, float fov) {
    void* ret = _ReturnAddress();
    DumpCamera("PCameraNode::setFOV (node)", node, fov, ret);
    o_SetFovNode(node, Widen(node + 0x1E0, fov));
    ApplyEngineHead(node, ret);
}

// ---- render resolution: graphicconfig.txt RESX / RESY (reader Engine.dll 0x563320, writer 0x563890; config struct: +4 monitor,
//      +8 RESX, +0xC RESY, +0x10 refresh, +0x14 windowed). The game sizes its window, swap chain and scene targets from these, so
//      for VR they are replaced after reading by the headset's per-eye size (d3d11hooks DecideRenderSize); writing the file back
//      puts the user's own values in (the flat game keeps its resolution).
typedef bool(__fastcall* CfgIo_t)(uint8_t* cfg);
static CfgIo_t o_CfgRead = nullptr, o_CfgWrite = nullptr;
static int g_cfgOrigW = 0, g_cfgOrigH = 0, g_cfgW = 0, g_cfgH = 0, g_cfgOrigWin = -1, g_cfgWin = 0;
bool __fastcall Hk_CfgRead(uint8_t* cfg) {
    bool ok = o_CfgRead(cfg);
    int* res = (int*)(cfg + 8);
    LOGI("greedfall: graphicconfig.txt read (%d): %dx%d, windowed %d", (int)ok, res[0], res[1], *(int*)(cfg + 0x14));
    if (ok && res[0] > 0 && res[1] > 0) {
        UINT w = 0, h = 0;
        D3D11RenderSize((UINT)res[0], (UINT)res[1], &w, &h);
        if (w && h) {
            g_cfgOrigW = res[0]; g_cfgOrigH = res[1]; g_cfgW = (int)w; g_cfgH = (int)h;
            res[0] = (int)w; res[1] = (int)h;
            int* win = (int*)(cfg + 0x14);   // 2 = windowed fullscreen uses the desktop size, 0 = fullscreen a display mode: windowed
            g_cfgOrigWin = *win; if (*win != 1) { *win = 1; g_cfgWin = 1; }
            LOGI("greedfall: game resolution %dx%d -> %ux%u for VR (the file keeps %dx%d)", g_cfgOrigW, g_cfgOrigH, w, h, g_cfgOrigW, g_cfgOrigH);
        }
    }
    return ok;
}
bool __fastcall Hk_CfgWrite(uint8_t* cfg) {
    int* res = (int*)(cfg + 8);
    bool swap = g_cfgW && res[0] == g_cfgW && res[1] == g_cfgH;
    int* win = (int*)(cfg + 0x14);
    bool swapWin = g_cfgWin && *win == 1 && g_cfgOrigWin >= 0;
    if (swap) { res[0] = g_cfgOrigW; res[1] = g_cfgOrigH; }
    if (swapWin) *win = g_cfgOrigWin;
    bool ok = o_CfgWrite(cfg);
    if (swap) { res[0] = g_cfgW; res[1] = g_cfgH; }
    if (swapWin) *win = 1;
    return ok;
}
void HookEngine() {
    if (o_GetDynamicRes || !g_engine) return;
    if (BytesMatch(g_engine, 0x568530, "f30f1041100f2ec1") && BytesMatch(g_engine, 0x568550, "4881c1e0010000"))
    {
        Hook((uint8_t*)g_engine + 0x568530, (void*)&Hk_SetFov, (void**)&o_SetFov, "PCamera::setFOV (culling)");
        Hook((uint8_t*)g_engine + 0x568550, (void*)&Hk_SetFovNode, (void**)&o_SetFovNode, "PCameraNode::setFOV (culling)");
        if (o_SetFovNode && BytesMatch(g_engine, kRenderCamCaller - 5, "e8")) G().engineHeadHooked = true;   // the call site is there
        else LOGW("greedfall: render camera call site differs - engine head off (head rotation in the eye matrices only)");
    } else LOGW("greedfall: setFOV bytes differ - culling FOV not widened");
    if (G().s.cullAim && BytesMatch(g_engine, 0x7A150, "4883ec5866410f70583055488bc2"))   // experiment only (test 065: not the culling)
        { void* stub = MakeMatMulStub(); if (stub) Hook((uint8_t*)g_engine + 0x7A150, stub, (void**)&o_MatMul, "4x4 multiply (culling camera)"); }
    if (BytesMatch(g_engine, 0x567270, "48895c2408574883ec20488bda"))
        { void* stub = MakeStub((void*)&Hk_CamCopy, true); if (stub) Hook((uint8_t*)g_engine + 0x567270, stub, (void**)&o_CamCopy, "camera snapshot copy"); g_camCopyOk = o_CamCopy != nullptr; }
    else LOGW("greedfall: camera snapshot bytes differ - the game camera cannot follow the head (culling only widened)");
    if (BytesMatch(g_engine, 0x563320, "40554881ec20040000") && BytesMatch(g_engine, 0x563890, "48895c240857"))
    {
        Hook((uint8_t*)g_engine + 0x563320, (void*)&Hk_CfgRead, (void**)&o_CfgRead, "graphicconfig.txt reader (render resolution)");
        Hook((uint8_t*)g_engine + 0x563890, (void*)&Hk_CfgWrite, (void**)&o_CfgWrite, "graphicconfig.txt writer");
    } else LOGW("greedfall: graphicconfig reader bytes differ - the game keeps its own resolution");
    if (BytesMatch(g_engine, 0x376E90, "4883ec28e8"))
        Hook((uint8_t*)g_engine + 0x376E90, (void*)&Hk_GetDynamicRes, (void**)&o_GetDynamicRes, "RENDER\\DynamicRes getter");
    else LOGW("greedfall: DynamicRes getter bytes differ (game updated?) - dynamic resolution not forced off");
}
DWORD WINAPI InstallThread(LPVOID) {
    for (int i = 0; i < 600 && !(g_engine && g_game); ++i) {   // Game.dll is loaded by the engine after start-up
        if (!g_engine) g_engine = GetModuleHandleW(L"Engine.dll");
        if (!g_game) g_game = GetModuleHandleW(L"Game.dll");
        if (!(g_engine && g_game)) Sleep(100);
    }
    if (!g_engine || !g_game) { LOGW("greedfall: Engine.dll / Game.dll not found - no GreedFall fixes"); return 0; }
    LOGI("greedfall: Engine.dll %p, Game.dll %p", (void*)g_engine, (void*)g_game);
    HookEngine();
    if (BytesMatch(g_game, 0x4339F0, "488bc4488958105548"))
        Hook((uint8_t*)g_game + 0x4339F0, (void*)&Hk_CamUpdate, (void**)&o_CamUpdate, "camera bob update");
    else LOGW("greedfall: camera update bytes differ (game updated?) - head bob not removed");
    // the shake switch: check the debug-variable record that points at it (name pointer + accessor)
    const uint8_t* rec = (const uint8_t*)g_game + 0xD68210;
    const char* nm = *(const char* const*)rec;
    if (nm && !IsBadReadPtr(nm, 33) && strcmp(nm, "ACTIVATION\\DisableAllCameraShake") == 0) {
        g_shakeOff = (volatile uint8_t*)g_game + 0xE54890;
        LOGI("greedfall: camera shake switch at %p (value %d)", (void*)g_shakeOff, *g_shakeOff);
    } else LOGW("greedfall: camera shake switch not found - shakes stay on");
    GreedfallTick();
    return 0;
}

} // namespace

void InstallGreedfallFixes() {
    g_engine = GetModuleHandleW(L"Engine.dll");   // a static import of the exe, loaded before us: hook its settings now
    HookEngine();
    if (HANDLE t = CreateThread(nullptr, 0, InstallThread, nullptr, 0, nullptr)) CloseHandle(t);
}

// Game settings by their registered accessors (records in Engine.dll's settings table: name, type, getter, setter;
// the getter writes the value to *out, the setter reads it from *in, the first argument is unused).
//   OPTION\TAA                 bool   get 0x34E860  set 0x34E830
//   TAA\TAAHistoryBlendFactor  float  get 0x34F6D0  set 0x34F690 (clamped by the game)
typedef int(__fastcall* SettingGet_t)(void* out);
typedef int(__fastcall* SettingSet_t)(void* unused, const void* in);
static int g_taaState = 0;   // 0 not checked, 1 accessors ok, -1 bytes differ (other game build)
static std::string g_taaNote = "not applied";
static void ApplyTaaSettings() {
    if (!g_engine) return;
    if (g_taaState == 0)
        g_taaState = BytesMatch(g_engine, 0x34E860, "488b05") && BytesMatch(g_engine, 0x34E830, "4883ec28488b0d") &&
                     BytesMatch(g_engine, 0x34F6D0, "488b05") && BytesMatch(g_engine, 0x34F690, "488b05") ? 1 : -1;
    if (g_taaState != 1) { g_taaNote = "accessors not found (game build differs)"; return; }
    char b[160];
    uint8_t taa = 0;
    ((SettingGet_t)((uint8_t*)g_engine + 0x34E860))(&taa);
    static int gameValue = -1;   // the game's own setting, restored when the menu switch is turned back off
    static bool forcedOff = false;
    if (gameValue < 0 && !forcedOff) gameValue = taa;
    int want = G().s.gfTaaOff ? 0 : G().s.gfTaa >= 0 ? G().s.gfTaa : (forcedOff ? gameValue : -1);
    if (!G().s.gfTaaOff && forcedOff && G().s.gfTaa < 0) forcedOff = false;
    if (G().s.gfTaaOff) forcedOff = true;
    if (want >= 0 && taa != (uint8_t)want) {
        uint8_t v = (uint8_t)want;
        ((SettingSet_t)((uint8_t*)g_engine + 0x34E830))(nullptr, &v);
        ((SettingGet_t)((uint8_t*)g_engine + 0x34E860))(&taa);
        LOGI("greedfall: TAA set %s (now %d)", v ? "on" : "off", taa);
    }
    float blend = -1;
    ((SettingGet_t)((uint8_t*)g_engine + 0x34F6D0))(&blend);
    if (G().s.gfTaaBlend >= 0 && std::fabs(blend - G().s.gfTaaBlend) > 1e-4f) {
        float v = G().s.gfTaaBlend;
        ((SettingSet_t)((uint8_t*)g_engine + 0x34F690))(nullptr, &v);
        ((SettingGet_t)((uint8_t*)g_engine + 0x34F6D0))(&blend);
        LOGI("greedfall: TAA history blend set %.3f (now %.3f)", v, blend);
    }
    // OPTION\SSAOQuality (record type 0x0a, int): get 0x34E6D0, set 0x34E6A0
    int ssao = -1;
    static int ssaoOk = -1;
    if (ssaoOk < 0) ssaoOk = BytesMatch(g_engine, 0x34E6D0, "488b05") && BytesMatch(g_engine, 0x34E6A0, "4883ec28488b0d") ? 1 : 0;
    if (ssaoOk == 1) {
        ((SettingGet_t)((uint8_t*)g_engine + 0x34E6D0))(&ssao);
        static int lastLogged = -99;
        if (ssao != lastLogged) { lastLogged = ssao; LOGI("greedfall: game SSAO quality %d", ssao); }
        if (G().s.gfSsaoQuality >= 0 && ssao != G().s.gfSsaoQuality) {
            int v = G().s.gfSsaoQuality;
            ((SettingSet_t)((uint8_t*)g_engine + 0x34E6A0))(nullptr, &v);
            ((SettingGet_t)((uint8_t*)g_engine + 0x34E6D0))(&ssao);
            LOGI("greedfall: SSAO quality set %d (now %d)", v, ssao);
        }
    }
    // OPTION\FXAA (record type 9, bool like TAA): get 0x34E810, set 0x34E7E0
    bool fxaa = false;
    static int fxOk = -1;
    if (fxOk < 0) fxOk = BytesMatch(g_engine, 0x34E810, "488b05") && BytesMatch(g_engine, 0x34E7E0, "4883ec28488b0d") ? 1 : 0;
    if (fxOk == 1) {
        ((SettingGet_t)((uint8_t*)g_engine + 0x34E810))(&fxaa);
        if (G().s.gfFxaa >= 0 && fxaa != (G().s.gfFxaa > 0)) {
            bool v = G().s.gfFxaa > 0;
            ((SettingSet_t)((uint8_t*)g_engine + 0x34E7E0))(nullptr, &v);
            ((SettingGet_t)((uint8_t*)g_engine + 0x34E810))(&fxaa);
            LOGI("greedfall: FXAA set %d (now %d)", (int)v, (int)fxaa);
        }
    }
    // OPTION\LodDistanceQuality (int, the options menu's draw distance): get 0x34E960, set 0x34E930
    int lod = -1;
    static int lodOk = -1;
    if (lodOk < 0) lodOk = BytesMatch(g_engine, 0x34E960, "488b05") && BytesMatch(g_engine, 0x34E930, "4883ec28488b0d") ? 1 : 0;
    if (lodOk == 1) {
        ((SettingGet_t)((uint8_t*)g_engine + 0x34E960))(&lod);
        static int lastLod = -99;
        if (lod != lastLod) { lastLod = lod; LOGI("greedfall: game draw distance (LodDistanceQuality) %d", lod); }
        if (G().s.gfLodDist >= 0 && lod != G().s.gfLodDist) {
            int v = G().s.gfLodDist;
            ((SettingSet_t)((uint8_t*)g_engine + 0x34E930))(nullptr, &v);
            ((SettingGet_t)((uint8_t*)g_engine + 0x34E960))(&lod);
            LOGI("greedfall: draw distance set %d (now %d)", v, lod);
        }
    }
    snprintf(b, sizeof b, "TAA %s, FXAA %s, history blend %.3f, SSAO quality %d, draw distance %d", taa ? "on" : "off", fxaa ? "on" : "off", blend, ssao, lod);
    g_taaNote = b;
}

void GreedfallTick() {
    if (g_shakeOff) {
        uint8_t want = G().s.gfNoCameraShake ? 1 : 0;
        if (*g_shakeOff != want) *g_shakeOff = want;
    }
    // (RENDER\EnableDOF, get 0x350130 / set 0x350100, was tried for game_dof: no effect on cutscene DoF, test 109.
    //  game_dof is handled on the DoF shader's constants in d3d11hooks.cpp.)
    static long last = -1000;
    if (G().frame - last >= 30) { last = G().frame; ApplyTaaSettings(); }   // the game rewrites them from its options at load
}

const char* GreedfallStatus() {
    static char buf[520];
    static long lastCalls = 0;
    long ccRender = 0, ccAll = GfCamCopyStats(&ccRender), recHits = 0, recMiss = 0;
    StereoCullRecStats(&recHits, &recMiss);
    long calls = g_setFovCalls;
    snprintf(buf, sizeof buf, "bob hook %s (%ld calls), dynres hook %s (%ld calls), shake switch %s, %s; setFOV %ld calls in 10 s, head culling factor %.2f, engine head %s (%ld frames); "
             "camera snapshots %ld (render camera %ld), game camera follows head %s (eyes matched to a turned snapshot %ld, unmatched cameras %ld), culling factor %.2f",
             o_CamUpdate ? "on" : "OFF", g_bobCalls, o_GetDynamicRes ? "on" : "OFF", g_dynResCalls, g_shakeOff ? "found" : "MISSING", g_taaNote.c_str(),
             calls - lastCalls, g_lastHeadK, G().engineHeadHooked && G().s.engineHead ? "on" : "off", (long)g_camHeadApplied,
             ccAll, ccRender, G().s.engineHead ? (g_camCopyOk ? "on" : "UNAVAILABLE") : "off", recHits, recMiss, g_lastK);
    lastCalls = calls;
    return buf;
}

} // namespace fvr
