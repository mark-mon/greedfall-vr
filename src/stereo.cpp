// UEVR3 stereo rendering for Unreal Engine 3 / D3D9 games (first done for The Last Remnant) - adapted from the FF XIII mod.
//
// UE3 shader constants (from the CTAB of every shader; verified on capture 66871):
//   VS  ViewProjectionMatrix c0 (4 rows, row-vector: clip = world * VP), CameraPosition c4, LocalToWorld separate,
//       ScreenToWorld (height fog, 3 rows = rows 0..2 of Q * inverse(VP)), CameraWorldPosition (particles).
//   PS  ScreenToShadowMatrix (modulated shadows) = Q * inverse(VP) * ShadowMatrix, ScreenToWorld, CameraPosition.
// UE3 view space is left-handed: x right, y up, +z forward, w_clip = z_view (P[2][3] = 1); near plane 10, infinite
// far with P22 = 0.999 (Z_PRECISION). 1 unit = 1 cm. VP is decomposed exactly into V (rigid) and P, so every eye
// matrix is rebuilt from the game's own VP: VP_eye = V * E_eye * P_eye, with E the headset eye transform (OpenXR
// is right-handed: converted with S = diag(1, 1, -1)). Q = [1 0 0 0; 0 1 0 0; 0 0 P22 1; 0 0 P32 0] maps
// (x*w, y*w, w, 1) back to clip space.
//
// Frame phases (UE3 in this game):
//   1. 3D scene into the scene targets (FP16 scene colour + D24S8, light attenuation, post) -> patched per eye.
//   2. First full-screen gamma composite into the back buffer (pixel shader with InverseGamma + SceneColorTexture):
//      drawn into both eye textures (left: game textures, right: twins) = the eye images.
//   3. "UI scenes" (the minimap) render into the scene targets again with their own camera: drawn into both eyes
//      unpatched, then composited again (2. repeats; the eye images are refreshed).
//   4. Canvas UI into the back buffer: repeated into the transparent HUD texture.
#include "stereo.h"
#include "d3d9hooks.h"
#include "capture.h"
#include "globals.h"
#include "imageio.h"
#include "xrbridge/client.h"
#include "rtscale.h"
#include <unordered_map>
#include <cmath>
#include <cstring>
#include <functional>

namespace fvr {

static bool Native() { return G().s.vrMethod == VR_NATIVE; }

// ------------------------------------------------------------------ 4x4 math (row-vector)
struct M4 { float m[4][4]; };
static M4 Mul(const M4& a, const M4& b) {
    M4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
    return r;
}
static M4 Transpose(const M4& a) {
    M4 r;
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) r.m[i][j] = a.m[j][i];
    return r;
}
static bool Inverse(const M4& a, M4* out) {
    double m[4][8];
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) { m[i][j] = a.m[i][j]; m[i][j + 4] = i == j; }
    for (int c = 0; c < 4; ++c) {
        int p = c;
        for (int r = c + 1; r < 4; ++r) if (fabs(m[r][c]) > fabs(m[p][c])) p = r;
        if (fabs(m[p][c]) < 1e-12) return false;
        for (int j = 0; j < 8; ++j) { double t = m[c][j]; m[c][j] = m[p][j]; m[p][j] = t; }
        double d = m[c][c];
        for (int j = 0; j < 8; ++j) m[c][j] /= d;
        for (int r = 0; r < 4; ++r)
            if (r != c) { double f = m[r][c]; for (int j = 0; j < 8; ++j) m[r][j] -= f * m[c][j]; }
    }
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) out->m[i][j] = (float)m[i][j + 4];
    return true;
}
static M4 Identity() { M4 r{}; r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1; return r; }
static M4 Load(int reg) { M4 r; memcpy(r.m, Sh().vsc[reg], sizeof(r.m)); return r; }

static M4 LoadPS(int reg) { M4 r; memcpy(r.m, Sh().psc[reg], sizeof(r.m)); return r; }

// ------------------------------------------------------------------ shader constant layouts (from the CTAB)
struct Layout { int vp = -1, cam = -1, camw = -1, s2w = -1, s2wN = 0, ivp = -1; };   // vertex shader
struct PSLayout { int s2s = -1, s2w = -1, s2wN = 0, cam = -1, vp = -1, ivp = -1; bool gamma = false; }; // pixel shader
static std::unordered_map<void*, Layout> g_layouts;
static std::unordered_map<void*, PSLayout> g_psLayouts;
static SRWLOCK g_layoutLock = SRWLOCK_INIT;
static Layout g_cur;             // layout of the current vertex shader
static PSLayout g_curPS;         // layout of the current pixel shader

// Calls f(name, registerSet, index, count) for every constant in the shader's CTAB.
template <class F> static void ForEachConstant(const DWORD* code, F f) {
    for (size_t i = 1; i < 65536;) {
        DWORD t = code[i];
        if (t == 0x0000FFFF) break;
        if ((t & 0xFFFF) == 0xFFFE) {
            DWORD len = (t >> 16) & 0x7FFF;
            if (len >= 8 && code[i + 1] == 0x42415443 /*'CTAB'*/) {
                const uint8_t* base = (const uint8_t*)&code[i + 2];
                size_t size = (len - 1) * 4;
                struct Hdr { DWORD Size, Creator, Version, Constants, ConstantInfo, Flags, Target; };
                struct Info { DWORD Name; WORD RegisterSet, RegisterIndex, RegisterCount, Reserved; DWORD TypeInfo, DefaultValue; };
                const Hdr* h = (const Hdr*)base;
                if (h->ConstantInfo + h->Constants * sizeof(Info) <= size) {
                    const Info* ci = (const Info*)(base + h->ConstantInfo);
                    for (DWORD k = 0; k < h->Constants; ++k)
                        if (ci[k].Name < size) f((const char*)base + ci[k].Name, (int)ci[k].RegisterSet, (int)ci[k].RegisterIndex, (int)ci[k].RegisterCount);
                }
                return;
            }
            i += 1 + len;
        } else i += 1 + ((t >> 24) & 0xF);
    }
}

void StereoRegisterVS(void* shader, const DWORD* code) {
    if (!shader || !code) return;
    Layout L;
    ForEachConstant(code, [&](const char* n, int set, int reg, int cnt) {
        if (set != 2 /*D3DXRS_FLOAT4*/) return;
        if (!strcmp(n, "ViewProjectionMatrix") && cnt == 4 && reg + 4 <= 256) L.vp = reg;
        else if ((!strcmp(n, "CameraPosition") || !strcmp(n, "CameraWorldPos") || !strcmp(n, "ViewOrigin")) && cnt >= 1 && reg < 256) L.cam = reg;
        else if (!strcmp(n, "CameraWorldPosition") && cnt >= 1 && reg < 256) L.camw = reg;
        else if (!strcmp(n, "InvViewProjectionMatrix") && cnt == 4 && reg + 4 <= 256) L.ivp = reg;
        else if (!strcmp(n, "ScreenToWorld") && cnt >= 3 && reg + cnt <= 256) { L.s2w = reg; L.s2wN = cnt > 4 ? 4 : cnt; }
    });
    bool any = L.vp >= 0 || L.cam >= 0 || L.camw >= 0 || L.s2w >= 0 || L.ivp >= 0;
    AcquireSRWLockExclusive(&g_layoutLock);
    if (any) g_layouts[shader] = L; else g_layouts.erase(shader);
    ReleaseSRWLockExclusive(&g_layoutLock);
}

void StereoRegisterPS(void* shader, const DWORD* code) {
    if (!shader || !code) return;
    PSLayout L;
    bool invGamma = false, sceneTex = false;
    ForEachConstant(code, [&](const char* n, int set, int reg, int cnt) {
        if (!strcmp(n, "SceneColorTexture") && set == 3 /*sampler*/ && reg == 0) sceneTex = true;
        if (set != 2) return;
        if (!strcmp(n, "ScreenToShadowMatrix") && cnt == 4 && reg + 4 <= 224) L.s2s = reg;
        else if (!strcmp(n, "ScreenToWorld") && cnt >= 3 && reg + cnt <= 224) { L.s2w = reg; L.s2wN = cnt > 4 ? 4 : cnt; }
        else if ((!strcmp(n, "CameraPosition") || !strcmp(n, "CameraWorldPos") || !strcmp(n, "ViewOrigin")) && cnt >= 1 && reg < 224) L.cam = reg;
        else if (!strcmp(n, "ViewProjectionMatrix") && cnt == 4 && reg + 4 <= 224) L.vp = reg;
        else if (!strcmp(n, "InvViewProjectionMatrix") && cnt == 4 && reg + 4 <= 224) L.ivp = reg;
        else if (!strcmp(n, "InverseGamma")) invGamma = true;
    });
    L.gamma = invGamma && sceneTex;
    bool any = L.s2s >= 0 || L.s2w >= 0 || L.cam >= 0 || L.vp >= 0 || L.ivp >= 0 || L.gamma;
    AcquireSRWLockExclusive(&g_layoutLock);
    if (any) g_psLayouts[shader] = L; else g_psLayouts.erase(shader);
    ReleaseSRWLockExclusive(&g_layoutLock);
}

void StereoOnSetVS(void* shader) {
    AcquireSRWLockShared(&g_layoutLock);
    auto it = g_layouts.find(shader);
    g_cur = it != g_layouts.end() ? it->second : Layout();
    ReleaseSRWLockShared(&g_layoutLock);
}
void StereoOnSetPS(void* shader) {
    AcquireSRWLockShared(&g_layoutLock);
    auto it = g_psLayouts.find(shader);
    g_curPS = it != g_psLayouts.end() ? it->second : PSLayout();
    ReleaseSRWLockShared(&g_layoutLock);
}

// ------------------------------------------------------------------ right-eye twins (native)
// The twin is attached to the game's resource as IUnknown private data, so it lives exactly as long as the original.
static const GUID kTwinGuid = {0x6a3c1f2e, 0x51b7, 0x4f0e, {0x9d, 0x2a, 0x13, 0x77, 0xc4, 0x08, 0xe1, 0x5b}};
static int g_privAddRefs = -1;   // does GetPrivateData AddRef the IUnknown it returns? (probed once)
static unsigned g_twins = 0;
static bool g_twinCreateFailed = false;

template <class T> static T* Twin(T* r) {
    if (!r) return nullptr;
    IUnknown* p = nullptr;
    DWORD sz = sizeof(p);
    if (FAILED(r->GetPrivateData(kTwinGuid, &p, &sz)) || !p || sz != sizeof(p)) return nullptr;
    if (g_privAddRefs == 1) p->Release();
    return (T*)p;
}

template <class T> static void Attach(T* orig, IUnknown* twin) {
    // with D3DSPD_IUNKNOWN, pData is the interface itself (D3D AddRefs it and releases it with the resource)
    orig->SetPrivateData(kTwinGuid, twin, sizeof(IUnknown*), D3DSPD_IUNKNOWN);
    if (g_privAddRefs < 0) {
        twin->AddRef();
        ULONG before = twin->Release();
        IUnknown* p = nullptr;
        DWORD sz = sizeof(p);
        orig->GetPrivateData(kTwinGuid, &p, &sz);
        twin->AddRef();
        ULONG after = twin->Release();
        g_privAddRefs = after > before ? 1 : 0;
        if (g_privAddRefs == 1) twin->Release();
        LOGI("stereo: GetPrivateData %s the twin", g_privAddRefs ? "AddRefs" : "does not AddRef");
    }
}

// Stereo targets: everything that is not square. Square targets (shadow maps 1024^2, reflections 256^2, ...) are
// eye-independent or not worth doubling, and stay shared.
static bool WantTwin(UINT w, UINT h) {
    const Settings& s = G().s;
    return s.vrEnabled && s.stereo && s.vrMethod == VR_NATIVE && w != h && w >= 8 && h >= 4;
}

void StereoOnCreateTexture(IDirect3DDevice9* dev, IDirect3DTexture9* tex, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT f, D3DPOOL pool) {
    if (!tex || !(usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) || !WantTwin(w, h)) return;
    InternalScope in;
    IDirect3DTexture9* twin = nullptr;
    if (FAILED(dev->CreateTexture(w, h, levels, usage, f, pool, &twin, nullptr)) || !twin) {
        if (!g_twinCreateFailed) LOGE("stereo: twin texture %ux%u %s failed", w, h, FormatName(f));
        g_twinCreateFailed = true;
        return;
    }
    for (DWORD l = 0; l < tex->GetLevelCount() && l < twin->GetLevelCount(); ++l) {
        IDirect3DSurface9 *a = nullptr, *b = nullptr;
        if (SUCCEEDED(tex->GetSurfaceLevel(l, &a)) && SUCCEEDED(twin->GetSurfaceLevel(l, &b)) && a && b) Attach(a, b);
        if (a) a->Release();
        if (b) b->Release();
    }
    Attach(tex, twin);
    twin->Release();   // now owned by the original's private data
    g_twins++;
    LOGD("stereo: twin for texture %ux%u %s usage %lx", w, h, FormatName(f), usage);
}

void StereoOnCreateSurface(IDirect3DDevice9* dev, IDirect3DSurface9* s, bool depth, UINT w, UINT h, D3DFORMAT f,
                           D3DMULTISAMPLE_TYPE ms, DWORD q, BOOL flag) {
    if (!s || !WantTwin(w, h)) return;
    InternalScope in;
    IDirect3DSurface9* twin = nullptr;
    HRESULT hr = depth ? dev->CreateDepthStencilSurface(w, h, f, ms, q, flag, &twin, nullptr)
                       : dev->CreateRenderTarget(w, h, f, ms, q, flag, &twin, nullptr);
    if (FAILED(hr) || !twin) {
        if (!g_twinCreateFailed) LOGE("stereo: twin surface %ux%u %s failed %08lX", w, h, FormatName(f), hr);
        g_twinCreateFailed = true;
        return;
    }
    Attach(s, twin);
    twin->Release();
    g_twins++;
    LOGD("stereo: twin for %s surface %ux%u %s msaa %d", depth ? "depth" : "colour", w, h, FormatName(f), (int)ms);
}

// ------------------------------------------------------------------ bound state (shadows; the device is pure)
static IDirect3DSurface9* g_rt[4] = {};
static IDirect3DSurface9* g_rtTwin[4] = {};
static IDirect3DSurface9* g_ds = nullptr;
static IDirect3DSurface9* g_dsTwin = nullptr;
static IDirect3DBaseTexture9* g_tex[20] = {};      // 0..15 pixel samplers, 16..19 vertex samplers
static IDirect3DBaseTexture9* g_texTwin[20] = {};
static UINT g_rt0W = 0, g_rt0H = 0;
static bool g_rt0IsBB = false;
static bool IsBackBuffer(IDirect3DDevice9* dev, IDirect3DSurface9* s);
static D3DVIEWPORT9 g_vp{};
static RECT g_scissor{};

static DWORD StageId(int i) { return i < 16 ? (DWORD)i : D3DVERTEXTEXTURESAMPLER0 + (i - 16); }

void StereoOnSetRT(IDirect3DDevice9* dev, DWORD i, IDirect3DSurface9* s) {
    if (i >= 4) return;
    g_rt[i] = s;
    if (i == 0) g_rt0IsBB = IsBackBuffer(dev, s);
    g_rtTwin[i] = Native() ? Twin(s) : nullptr;
    if (i == 0) {
        g_rt0W = g_rt0H = 0;
        D3DSURFACE_DESC d;
        { InternalScope in; if (s && SUCCEEDED(s->GetDesc(&d))) { g_rt0W = d.Width; g_rt0H = d.Height; } }   // real size
        // SetRenderTarget(0) resets the viewport and scissor rect to the whole target (kept in the game's units)
        ScaleInfo si;
        UINT gw = g_rt0W, gh = g_rt0H;
        if (RtGetInfo(s, &si)) { gw = si.origW; gh = si.origH; }
        g_vp = {0, 0, gw, gh, 0.0f, 1.0f};
        g_scissor = {0, 0, (LONG)gw, (LONG)gh};
    }
}

// Our own viewport/scissor restores bypass the hooks, so they convert the game-unit shadows themselves.
static void ApplyViewport(IDirect3DDevice9* dev, IDirect3DSurface9* gameRt0) {
    ScaleInfo si;
    if (RtGetInfo(gameRt0, &si)) {
        D3DVIEWPORT9 v = RtScaleViewport(g_vp, si);
        RECT r = RtScaleRect(g_scissor, si);
        dev->SetViewport(&v);
        dev->SetScissorRect(&r);
    } else {
        dev->SetViewport(&g_vp);
        dev->SetScissorRect(&g_scissor);
    }
}
void StereoOnSetDS(IDirect3DSurface9* s) { g_ds = s; g_dsTwin = Native() ? Twin(s) : nullptr; }
void StereoOnSetTexture(DWORD stage, IDirect3DBaseTexture9* t) {
    int i = stage < 16 ? (int)stage : (stage >= D3DVERTEXTEXTURESAMPLER0 && stage <= D3DVERTEXTEXTURESAMPLER3) ? 16 + (int)(stage - D3DVERTEXTEXTURESAMPLER0) : -1;
    if (i < 0) return;
    g_tex[i] = t;
    g_texTwin[i] = Native() ? Twin(t) : nullptr;
}
void StereoOnViewport(const D3DVIEWPORT9* vp) { if (vp) g_vp = *vp; }
void StereoOnScissor(const RECT* r) { if (r) g_scissor = *r; }

// ------------------------------------------------------------------ eye / HUD textures
static UINT g_bbW = 0, g_bbH = 0;
static IDirect3DSurface9* g_uiTarget = nullptr;   // the game's UI/compose target (copied to the back buffer)
static bool g_uiPhase = false;                      // between the scene copy into g_uiTarget and its copy to the back buffer

static IDirect3DTexture9* g_eyeTex[2] = {};
static IDirect3DSurface9* g_eyeSurf[2] = {};
static IDirect3DTexture9* g_hudTex = nullptr;
static IDirect3DSurface9* g_hudSurf = nullptr;
static UINT g_texW = 0, g_texH = 0, g_hudW = 0, g_hudH = 0;

static void ReleaseTextures() {
    for (int e = 0; e < 2; ++e) {
        if (g_eyeSurf[e]) g_eyeSurf[e]->Release();
        if (g_eyeTex[e]) g_eyeTex[e]->Release();
        g_eyeSurf[e] = nullptr; g_eyeTex[e] = nullptr;
    }
    if (g_hudSurf) g_hudSurf->Release();
    if (g_hudTex) g_hudTex->Release();
    g_hudSurf = nullptr; g_hudTex = nullptr;
    g_texW = g_texH = 0;
}

// Eye and HUD textures at the size of the game's full-screen targets (the headset resolution when they are scaled;
// the HUD must match the UI target's pixel grid because UI draws use its stencil).
static bool EnsureTextures(IDirect3DDevice9* dev) {
    UINT ew = RtScaleActive() ? RtSceneW() : g_bbW, eh = RtScaleActive() ? RtSceneH() : g_bbH;
    if (g_texW == ew && g_texH == eh && g_hudSurf && g_hudW == g_bbW && g_hudH == g_bbH) return true;
    ReleaseTextures();
    InternalScope in;
    bool ok = true;
    for (int e = 0; e < 2 && ok; ++e)
        ok = SUCCEEDED(dev->CreateTexture(ew, eh, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_eyeTex[e], nullptr)) &&
             SUCCEEDED(g_eyeTex[e]->GetSurfaceLevel(0, &g_eyeSurf[e]));
    ok = ok && SUCCEEDED(dev->CreateTexture(g_bbW, g_bbH, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_hudTex, nullptr)) &&
         SUCCEEDED(g_hudTex->GetSurfaceLevel(0, &g_hudSurf));
    if (!ok) { LOGE("stereo: could not create eye/HUD textures %ux%u", ew, eh); ReleaseTextures(); return false; }
    g_texW = ew; g_texH = eh; g_hudW = g_bbW; g_hudH = g_bbH;
    LOGI("stereo: HUD texture %ux%u, eye textures %ux%u created", g_hudW, g_hudH, ew, eh);
    return true;
}

// ------------------------------------------------------------------ per-frame state
static bool g_active = false;      // stereo enabled for the frame being rendered
static int g_eye = 0;              // AER: eye of the frame being rendered
static M4 g_E[2];                  // UE3 view space (LH) -> eye space, per eye
static M4 g_Eprev[2];              // the previous frame's (for the game's previous-frame matrices: TAA / motion vectors)
static bool g_EprevOk = false;
static bool g_frameStereo = false; // this frame's scene was captured into the eye textures
static unsigned g_lastPatched = 0;
static bool g_lastStereo = false;  // the previous frame was a stereo frame: this frame's 3D draws get the eye cameras
static bool g_patchAllowed = true; // = g_lastStereo at frame start (or a stereo frame started this frame)
static bool g_wantStereo = false; // this frame qualified as a 3D scene but was drawn unpatched: patch from the next one
static bool g_eyeValid[2] = {};
static unsigned g_uiInScene = 0;    // UI-camera draws before this frame's scene composite (menus drawn into the 3D scene)
static unsigned g_patched = 0, g_hudDraws = 0, g_rightDraws = 0, g_mirrored = 0, g_composites = 0, g_hudScenes = 0;
static bool g_didPatch = false;    // constants of the current draw were changed
static float g_candP00 = 0, g_candP11 = 0;   // projection of this frame's first 3D view
static IDirect3DSurface9* g_filled[8];   // HUD layer: twins filled black for this frame's UI scenes
static int g_filledN = 0;
static bool HudLayer() { return G().s.hudLayer; }
static void FillTwinForHud(IDirect3DDevice9* dev);
static bool g_noPatch = false;     // current draw: right eye drawn with the game's constants (UI scenes)
// Set at this frame's first gamma composite into the back buffer. Before it: the 3D scene (patched); after it: UI
// scenes into the scene targets (both eyes, unpatched) and the canvas UI into the back buffer (HUD).
static bool g_sceneCopyThisFrame = false, g_sceneCopyLastFrame = false;

static xrb::IpcPose g_framePose[2];
static xrb::IpcFov g_frameFov[2];
static int64_t g_frameTime = 0;
static xrb::IpcPose g_eyePose[2];   // pose each eye texture was rendered with
static xrb::IpcFov g_eyeFov[2];
static int64_t g_eyeTime[2] = {};
static xrb::IpcFov g_gameFov{-0.785f, 0.785f, 0.44f, -0.44f};

static unsigned g_statFrames = 0, g_statStereo = 0, g_statPatched = 0, g_statHud = 0, g_statRight = 0, g_statMirror = 0, g_statComp = 0;

static void ReleaseHudPass();
void StereoOnReset() {
    ReleaseTextures();
    ReleaseHudPass();
    for (int e = 0; e < 2; ++e) g_eyeValid[e] = false;
    g_uiTarget = nullptr;
    g_uiPhase = false;
    memset(g_rt, 0, sizeof(g_rt)); memset(g_rtTwin, 0, sizeof(g_rtTwin));
    g_ds = g_dsTwin = nullptr;
    memset(g_tex, 0, sizeof(g_tex)); memset(g_texTwin, 0, sizeof(g_texTwin));
}

// Eye transform from an OpenXR eye pose (relative to the recentered reference = the game camera), in UE3's
// left-handed view space: E_lh = S * E_rh * S with S = diag(1, 1, -1, 1).
// Engine head: the game's render camera is turned with the head (greedfall.cpp camera hook), so the engine culls, LODs and
// shadows for where the player looks; the eye matrices then add only the eye offsets. g_headPose = this frame's head
// (eye midpoint, left eye orientation) in the recentred space.
static xrb::IpcPose g_headPose;
static bool g_engineHeadActive = false;
static volatile long g_engineHeadFrame = -100;   // last frame the camera hook applied the head
static void BuildEye(int e, const xrb::IpcPose& pin, const xrb::IpcPose& other) {
    const Settings& s = G().s;
    xrb::IpcPose p = pin;
    // ipd_scale: 0 = both eyes at the centre (mono, head tracking only), 1 = the headset's eye distance
    float k = s.ipdScale;
    p.px = 0.5f * (pin.px + other.px) + k * 0.5f * (pin.px - other.px);
    p.py = 0.5f * (pin.py + other.py) + k * 0.5f * (pin.py - other.py);
    p.pz = 0.5f * (pin.pz + other.pz) + k * 0.5f * (pin.pz - other.pz);
    float yawDeg = s.debugHeadYaw;
    if (s.debugHeadSweep != 0)   // test aid: head yaws back and forth (amplitude deg, 3 s period)
        yawDeg += s.debugHeadSweep * sinf((float)(GetTickCount64() % 3000) / 3000.f * 6.2831853f);
    if (yawDeg != 0) {   // test aid without a headset: extra yaw (deg) applied to the head
        float a = yawDeg * 3.14159265f / 180 / 2;
        float yy = sinf(a), yw = cosf(a);                          // q = q_yaw(0, yy, 0, yw) * q
        float qx = yw * p.qx + yy * p.qz, qy = yw * p.qy + yy * p.qw, qz = yw * p.qz - yy * p.qx, qw = yw * p.qw - yy * p.qy;
        p.qx = qx; p.qy = qy; p.qz = qz; p.qw = qw;
        float c = cosf(2 * a), sn = sinf(2 * a);                   // rotate the position too
        float px = c * p.px + sn * p.pz, pz = -sn * p.px + c * p.pz;
        p.px = px; p.pz = pz;
    }
    if (s.debugHeadPitch != 0) {   // test aid: extra pitch (deg, + = look up) applied to the head
        float a = s.debugHeadPitch * 3.14159265f / 180 / 2;
        float xx = sinf(a), xw = cosf(a);                          // q = q_pitch(xx, 0, 0, xw) * q
        float qx = xw * p.qx + xx * p.qw, qy = xw * p.qy - xx * p.qz, qz = xw * p.qz + xx * p.qy, qw = xw * p.qw - xx * p.qx;
        p.qx = qx; p.qy = qy; p.qz = qz; p.qw = qw;
        float c = cosf(2 * a), sn = sinf(2 * a);
        float py = c * p.py - sn * p.pz, pz = sn * p.py + c * p.pz;
        p.py = py; p.pz = pz;
    }
    if (s.engineHead && g_engineHeadActive) {
        // the head is already in the game camera (engine hook): the eye transform is only the eye relative to the head
        float hq[4] = {g_headPose.qx, g_headPose.qy, g_headPose.qz, g_headPose.qw}, ci[4] = {-hq[0], -hq[1], -hq[2], hq[3]};
        float d[3] = {(p.px - g_headPose.px), (p.py - g_headPose.py), (p.pz - g_headPose.pz)};
        // v' = conj(qh) * v * qh
        auto qmul = [](const float a[4], const float b[4], float o[4]) {
            o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
            o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
            o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
            o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
        };
        float v[4] = {d[0], d[1], d[2], 0}, t1[4], t2[4];
        qmul(ci, v, t1); qmul(t1, hq, t2);
        float eq[4] = {p.qx, p.qy, p.qz, p.qw}, rq[4];
        qmul(ci, eq, rq);
        p.px = t2[0]; p.py = t2[1]; p.pz = t2[2];
        p.qx = rq[0]; p.qy = rq[1]; p.qz = rq[2]; p.qw = rq[3];
    }
    if (!s.headTracking) { p.qx = p.qy = p.qz = 0; p.qw = 1; }   // eyes keep the game camera's orientation
    float sc = s.worldScale;
    float tx = p.px * sc, ty = p.py * sc, tz = p.pz * sc;
    if (!s.headPosition && !(s.engineHead && g_engineHeadActive)) {   // rotation only: keep just the eye offset from the head centre
        tx = (p.px - 0.5f * (pin.px + other.px)) * sc; ty = (p.py - 0.5f * (pin.py + other.py)) * sc; tz = (p.pz - 0.5f * (pin.pz + other.pz)) * sc;
    }
    float x = p.qx, y = p.qy, z = p.qz, w = p.qw;
    float R[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                     {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                     {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
    // RH: E = T(-t) * R4 (row-vector: v_eye = v_view * E). LH: conjugate with S (flip z).
    M4 R4 = Identity(), T = Identity();
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) R4.m[i][j] = R[i][j];
    T.m[3][0] = -tx; T.m[3][1] = -ty; T.m[3][2] = -tz;
    M4 Erh = Mul(T, R4), S = Identity();
    S.m[2][2] = -1;
    g_E[e] = Mul(Mul(S, Erh), S);
}

// ------------------------------------------------------------------ view decomposition (VP -> V, P) and eye matrices
struct EyeView {
    float key[16];
    bool persp = false;
    bool ui = false;                // UE3 UI-scene camera (identity rotation)
    M4 VP, V, P, Q, invQiVP;        // game: Q * inverse(VP) and its inverse
    M4 VPe[2], QiVPe[2], iVPe[2];   // per eye
    M4 Ve[2], Pe[2], iVe[2];        // per eye view / projection (VPe = Ve * Pe), inverse view
    float camE[2][3];               // eye world position
};
// D3D11: GreedFall records draws on 16 threads at once. The view cache and the per-frame eye transforms are shared, so
// every access (lookup / insert / copy-out, and the per-frame pose update) holds g_eyeLock. Before (a single-threaded
// D3D9 design), a draw could copy a slot another thread was refilling with another camera, or a half-updated pose ->
// geometry thrown through the camera ("rays") and objects popping while the head moved (user, build 12; test 060).
static const int kViews = 24;
static EyeView g_views[kViews];
static SRWLOCK g_eyeLock = SRWLOCK_INIT;
struct EyeLockGuard { EyeLockGuard() { AcquireSRWLockExclusive(&g_eyeLock); } ~EyeLockGuard() { ReleaseSRWLockExclusive(&g_eyeLock); } };
static int g_viewN = 0, g_viewNext = 0;
static bool g_haveViewVP = false;
static M4 g_viewVP;                  // VP of the last patched 3D draw (for passes that have no VP of their own)

static xrb::IpcFov g_prevFov[2];
// ---- head-aimed game camera (GreedFall, greedfall.cpp Hk_CamCopy): the engine's per-frame render-camera snapshot is
// turned with the head, so the engine culls / LODs / shadows where the player looks. Each turned snapshot is recorded
// (its view in the decomposition's convention + the rotation used); a decomposed camera that matches one gets
// E_adj = Rot^-1 * E for its eyes, so the eye views are exactly V * E as without the turn (no mismatch even when the
// snapshot used another frame's pose).
struct CullRec { M4 V; M4 R; };
static CullRec g_cullRec[8]; static int g_cullRecN = 0, g_cullRecNext = 0; static volatile long g_cullRecHits = 0, g_cullRecMiss = 0;
static M4 FlipZ(const M4& a) { M4 S = Identity(); S.m[2][2] = -1; return Mul(Mul(S, a), S); }
// rotation (decomposition convention) for the snapshot in the node's convention; flip = node view's z axis is opposite
bool StereoCullFlip(const float nodeVP[16], const float nodeView[16]) {
    float vz[3] = {nodeVP[3], nodeVP[7], nodeVP[11]};   // column 3 of VP = view column 2 * P23
    float d = vz[0] * nodeView[2] + vz[1] * nodeView[6] + vz[2] * nodeView[10];
    return d < 0;
}
void StereoRecordCullCopy(const float viewRotNode[16], const float rotDec[16], bool flip) {
    M4 V, R; memcpy(V.m, viewRotNode, 64); memcpy(R.m, rotDec, 64);
    if (flip) { M4 S = Identity(); S.m[2][2] = -1; V = Mul(V, S); }   // decomposition's view = node view * S
    EyeLockGuard lk;
    g_cullRec[g_cullRecNext] = {V, R}; g_cullRecNext = (g_cullRecNext + 1) % 8; if (g_cullRecN < 8) g_cullRecN++;
}
// node-convention rotation for a decomposition-convention rotation
void StereoCullRotToNode(const float rotDec[16], bool flip, float out[16]) {
    M4 R; memcpy(R.m, rotDec, 64); if (flip) R = FlipZ(R); memcpy(out, R.m, 64);
}
static const CullRec* FindCullRec(const M4& V) {
    for (int k = 0; k < g_cullRecN; ++k) {
        const M4& a = g_cullRec[(g_cullRecNext - 1 - k + 8) % 8].V;
        float tol = 1e-3f * (1 + fabsf(V.m[3][0]) + fabsf(V.m[3][1]) + fabsf(V.m[3][2]));
        bool ok = true;
        for (int i = 0; i < 4 && ok; ++i) for (int j = 0; j < 3 && ok; ++j) ok = fabsf(a.m[i][j] - V.m[i][j]) < (i == 3 ? tol : 2e-3f);
        if (ok) return &g_cullRec[(g_cullRecNext - 1 - k + 8) % 8];
    }
    return nullptr;
}
void StereoCullRecStats(long* hits, long* miss) { *hits = InterlockedExchange(&g_cullRecHits, 0); *miss = InterlockedExchange(&g_cullRecMiss, 0); }

static bool Decompose(const M4& VP, EyeView& ev, const M4* E = nullptr, const xrb::IpcFov* fovs = nullptr) {
    if (!E) E = g_E;
    if (!fovs) fovs = g_frameFov;
    // column 3 of VP = column 2 of V (P[2][3] = 1): the view's forward axis (unit length) + translation
    float vz[4] = {VP.m[0][3], VP.m[1][3], VP.m[2][3], VP.m[3][3]};
    float n = sqrtf(vz[0] * vz[0] + vz[1] * vz[1] + vz[2] * vz[2]);
    if (n < 0.98f || n > 1.02f) return false;   // orthographic or not a camera matrix
    for (int i = 0; i < 4; ++i) vz[i] /= n;
    float P23 = n;
    auto col = [&](int c, float* v) { for (int i = 0; i < 4; ++i) v[i] = VP.m[i][c]; };
    float c0[4], c1[4], c2[4];
    col(0, c0); col(1, c1); col(2, c2);
    float P20 = c0[0] * vz[0] + c0[1] * vz[1] + c0[2] * vz[2];
    float P21 = c1[0] * vz[0] + c1[1] * vz[1] + c1[2] * vz[2];
    float P22 = c2[0] * vz[0] + c2[1] * vz[1] + c2[2] * vz[2];
    float vx[4], vy[4];
    for (int i = 0; i < 4; ++i) { vx[i] = c0[i] - P20 * vz[i]; vy[i] = c1[i] - P21 * vz[i]; }
    float P00 = sqrtf(vx[0] * vx[0] + vx[1] * vx[1] + vx[2] * vx[2]);
    float P11 = sqrtf(vy[0] * vy[0] + vy[1] * vy[1] + vy[2] * vy[2]);
    if (P00 < 1e-4f || P11 < 1e-4f) return false;
    for (int i = 0; i < 4; ++i) { vx[i] /= P00; vy[i] /= P11; }
    float P32 = c2[3] - P22 * vz[3];
    M4 V = Identity(), P{};
    for (int i = 0; i < 4; ++i) { V.m[i][0] = vx[i]; V.m[i][1] = vy[i]; V.m[i][2] = vz[i]; V.m[i][3] = i == 3 ? 1.0f : 0.0f; }
    P.m[0][0] = P00; P.m[1][1] = P11; P.m[2][0] = P20; P.m[2][1] = P21; P.m[2][2] = P22; P.m[2][3] = P23; P.m[3][2] = P32;
    // UE3 UI scenes (title, menus, minimap) use a camera with exactly the identity rotation, placed so that pixel
    // coordinates map to the screen: not a world camera, never patched (they stay flat / head-locked).
    {
        float dev = 0;
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) dev = fmaxf(dev, fabsf(V.m[i][j] - (i == j ? 1.0f : 0.0f)));
        if (dev < 1e-5f) {
            static int logged = 0;
            if (logged++ < 2) LOGI("stereo: UI camera (identity rotation, at %.1f %.1f %.1f) - not patched", -V.m[3][0], -V.m[3][1], -V.m[3][2]);
            ev.ui = true;
            return false;
        }
    }
    M4 Eadj[2];
    if (g_cullRecN && G().s.engineHead) {
        if (const CullRec* r = FindCullRec(V)) {
            M4 Rt = Identity(); for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) Rt.m[i][j] = r->R.m[j][i];
            Eadj[0] = Mul(Rt, E[0]); Eadj[1] = Mul(Rt, E[1]); E = Eadj;
            InterlockedIncrement(&g_cullRecHits);
        } else InterlockedIncrement(&g_cullRecMiss);
    }
    ev.VP = VP; ev.V = V; ev.P = P;
    ev.Q = Identity();
    ev.Q.m[2][2] = P22; ev.Q.m[2][3] = 1; ev.Q.m[3][2] = P32; ev.Q.m[3][3] = 0;
    M4 iVP;
    if (!Inverse(VP, &iVP) || !Inverse(Mul(ev.Q, iVP), &ev.invQiVP)) return false;
    // game FOV (for fov_mode 0 and logging): LH, ndc.x = P00 * x/z + P20 -> edges at tan = (+-1 - P20) / P00
    for (int e = 0; e < 2; ++e) {
        M4 Pe = P;
        if (G().s.fovMode == 1) {
            const xrb::IpcFov& f = fovs[e];
            float L = tanf(f.left), R = tanf(f.right), U = tanf(f.up), D = tanf(f.down);
            Pe.m[0][0] = 2 / (R - L) * P23;
            Pe.m[1][1] = 2 / (U - D) * P23;
            Pe.m[2][0] = -(R + L) / (R - L) * P23;
            Pe.m[2][1] = -(U + D) / (U - D) * P23;
            if (G().s.keepJitter && fabsf(P.m[2][0]) < 0.01f && fabsf(P.m[2][1]) < 0.01f) {   // the game's TAA sub-pixel jitter
                Pe.m[2][0] += P.m[2][0];
                Pe.m[2][1] += P.m[2][1];
            }
        }
        M4 Ve = Mul(V, E[e]);
        ev.VPe[e] = Mul(Ve, Pe);
        ev.Ve[e] = Ve; ev.Pe[e] = Pe;
        M4 iVe, iVPe;
        if (!Inverse(Ve, &iVe) || !Inverse(ev.VPe[e], &iVPe)) return false;
        ev.QiVPe[e] = Mul(ev.Q, iVPe);
        ev.iVe[e] = iVe;
        ev.iVPe[e] = iVPe;
        for (int i = 0; i < 3; ++i) ev.camE[e][i] = iVe.m[3][i];
    }
    // cameras narrower than stereo_min_fov (The Last Remnant's title backdrop: 20 deg) are not 3D scenes to patch
    if (G().s.stereoMinFov > 0 && 2 * atanf(P23 / P.m[0][0]) * 57.2958f < G().s.stereoMinFov) return false;
    ev.persp = true;
    return true;
}

static EyeView* GetView(const M4& VP) {
    for (int i = 0; i < g_viewN; ++i)
        if (!memcmp(g_views[i].key, VP.m, sizeof(g_views[i].key))) {
            if (g_views[i].ui && !g_sceneCopyThisFrame) g_uiInScene++;   // a UI-scene draw before the scene composite
            return g_views[i].persp ? &g_views[i] : nullptr;
        }
    EyeView& ev = g_views[g_viewNext];
    g_viewNext = (g_viewNext + 1) % kViews;
    if (g_viewN < kViews) g_viewN++;
    memcpy(ev.key, VP.m, sizeof(ev.key));
    ev.persp = false;
    ev.ui = false;
    if (!Decompose(VP, ev)) { if (ev.ui && !g_sceneCopyThisFrame) g_uiInScene++; return nullptr; }
    static int logged = 0;
    if (logged < 3) {
        logged++;
        const M4& P = ev.P;
        LOGI("stereo: view %d: P00 %.4f P11 %.4f P20 %.4f P21 %.4f P22 %.5f P32 %.3f (FOV %.1f x %.1f deg), camera %.0f %.0f %.0f", logged,
             P.m[0][0], P.m[1][1], P.m[2][0], P.m[2][1], P.m[2][2], P.m[3][2], 2 * atanf(1 / P.m[0][0]) * 57.2958f,
             2 * atanf(1 / P.m[1][1]) * 57.2958f, ev.camE[0][0], ev.camE[0][1], ev.camE[0][2]);
    }
    return &ev;
}

static void InvalidateViews() { g_viewN = 0; g_viewNext = 0; }

void StereoBeginFrame(IDirect3DDevice9* dev) {
    g_statFrames++;
    if (g_frameStereo) g_statStereo++;
    g_statPatched += g_patched; g_statHud += g_hudDraws; g_statRight += g_rightDraws; g_statMirror += g_mirrored; g_statComp += g_composites;
    g_lastStereo = g_frameStereo;
    g_lastPatched = g_patched;
    g_patchAllowed = g_frameStereo || g_wantStereo;
    g_wantStereo = false;
    g_frameStereo = false;
    g_sceneCopyLastFrame = g_sceneCopyThisFrame;
    g_sceneCopyThisFrame = false;
    g_patched = g_hudDraws = g_rightDraws = g_mirrored = g_composites = 0;
    g_uiInScene = 0;
    g_uiPhase = false;
    g_active = false;
    g_haveViewVP = false;
    g_candP00 = g_candP11 = 0;
    g_filledN = 0;
    InvalidateViews();
    if (!G().s.stereo || !xrb::ClientHostConnected()) return;
    IDirect3DSurface9* bb = nullptr;
    if (!(bb = GameBackBuffer(dev))) return;
    D3DSURFACE_DESC d;
    bb->GetDesc(&d);
    bb->Release();
    g_bbW = d.Width; g_bbH = d.Height;
    if (!EnsureTextures(dev)) return;
    xrb::IpcPose pose[2];
    xrb::IpcFov fov[2];
    int64_t t = 0;
    if (!xrb::ClientGetLatestPoses(pose, fov, &t)) return;
    g_eye = (int)(G().frame & 1);
    memcpy(g_framePose, pose, sizeof(pose));
    memcpy(g_frameFov, fov, sizeof(fov));
    g_frameTime = t;
    g_headPose = pose[0];
    g_headPose.px = 0.5f * (pose[0].px + pose[1].px); g_headPose.py = 0.5f * (pose[0].py + pose[1].py); g_headPose.pz = 0.5f * (pose[0].pz + pose[1].pz);
    // only while the camera hook really applied it (last 2 frames); otherwise the eyes carry the head rotation as before
    g_engineHeadActive = G().s.engineHead && G().engineHeadHooked && G().frame - g_engineHeadFrame <= 2;
    BuildEye(0, pose[0], pose[1]);
    BuildEye(1, pose[1], pose[0]);
    g_active = true;
}

// The game's own projection of the main 3D view (for the culling FOV search, camfov.cpp).
static float g_gameP00 = 0, g_gameP11 = 0;
static long g_gamePFrame = -1;
bool StereoGameProjection(float* p00, float* p11, long* frame) {
    if (g_gamePFrame < 0) return false;
    *p00 = g_gameP00; *p11 = g_gameP11; *frame = g_gamePFrame;
    return true;
}

// Uploads the eye's camera constants for the current draw (VS and PS). Returns true if anything was patched.
static int g_patchedRegsVS[8], g_patchedCntVS[8], g_nVS = 0;
static int g_patchedRegsPS[8], g_patchedCntPS[8], g_nPS = 0;
static void Patch(IDirect3DDevice9* dev, int eye) {
    if (g_noPatch) return;
    bool needView = g_cur.vp >= 0 || g_cur.cam >= 0 || g_cur.camw >= 0 || g_cur.s2w >= 0 || g_cur.ivp >= 0 || g_curPS.s2s >= 0 ||
                    g_curPS.s2w >= 0 || g_curPS.cam >= 0 || g_curPS.vp >= 0 || g_curPS.ivp >= 0;
    if (!needView) return;
    EyeView* ev = nullptr;
    if (g_cur.vp >= 0) {
        M4 VP = Load(g_cur.vp);
        ev = GetView(VP);
        if (ev && eye == 0) {
            if (!g_haveViewVP) {   // the frame's first 3D view = the main camera (published if the frame is a real scene)
                g_candP00 = ev->P.m[0][0] / ev->P.m[2][3]; g_candP11 = ev->P.m[1][1] / ev->P.m[2][3];
            }
            g_viewVP = VP; g_haveViewVP = true;
        }
    } else if (g_haveViewVP) ev = GetView(g_viewVP);
    if (!ev) return;
    if (!g_patchAllowed) { g_nVS = g_nPS = 0; g_didPatch = true; return; }   // counted (stereo test), drawn with the game camera
    InternalScope in;
    g_nVS = g_nPS = 0;
    auto vs = [&](int reg, const float* d, int n) { dev->SetVertexShaderConstantF(reg, d, n); g_patchedRegsVS[g_nVS] = reg; g_patchedCntVS[g_nVS++] = n; };
    auto ps = [&](int reg, const float* d, int n) { dev->SetPixelShaderConstantF(reg, d, n); g_patchedRegsPS[g_nPS] = reg; g_patchedCntPS[g_nPS++] = n; };
    if (g_cur.vp >= 0) vs(g_cur.vp, &ev->VPe[eye].m[0][0], 4);
    if (g_cur.cam >= 0) { float c[4] = {ev->camE[eye][0], ev->camE[eye][1], ev->camE[eye][2], Sh().vsc[g_cur.cam][3]}; vs(g_cur.cam, c, 1); }
    if (g_cur.camw >= 0) { float c[4] = {ev->camE[eye][0], ev->camE[eye][1], ev->camE[eye][2], Sh().vsc[g_cur.camw][3]}; vs(g_cur.camw, c, 1); }
    if (g_cur.s2w >= 0 && G().s.dbgFogPatch) vs(g_cur.s2w, &ev->QiVPe[eye].m[0][0], g_cur.s2wN);
    if (g_curPS.s2s >= 0 && G().s.dbgShadowPatch) { M4 Me = Mul(ev->QiVPe[eye], Mul(ev->invQiVP, LoadPS(g_curPS.s2s))); ps(g_curPS.s2s, &Me.m[0][0], 4); }
    if (g_curPS.s2w >= 0 && G().s.dbgFogPatch) ps(g_curPS.s2w, &ev->QiVPe[eye].m[0][0], g_curPS.s2wN);
    if (g_curPS.cam >= 0) { float c[4] = {ev->camE[eye][0], ev->camE[eye][1], ev->camE[eye][2], Sh().psc[g_curPS.cam][3]}; ps(g_curPS.cam, c, 1); }
    if (g_cur.ivp >= 0) vs(g_cur.ivp, &ev->iVPe[eye].m[0][0], 4);
    if (g_curPS.vp >= 0) ps(g_curPS.vp, &ev->VPe[eye].m[0][0], 4);
    if (g_curPS.ivp >= 0) ps(g_curPS.ivp, &ev->iVPe[eye].m[0][0], 4);
    g_didPatch = true;
}

// Camera world position / rotation of the last main-scene view (for reprojection and the menu).
static float g_camPos[3] = {};
void StereoCameraPos(float out[3]) { memcpy(out, g_camPos, sizeof(g_camPos)); }
static float g_camRot[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
static float g_sceneRot[3][3];
static long g_sceneFrame = -1, g_eyeFrame[2] = {-1, -1};
bool StereoSceneCamera(float rot[3][3], long* frame, long eyeFrame[2]) {
    if (g_sceneFrame < 0) return false;
    memcpy(rot, g_sceneRot, sizeof(g_sceneRot));
    *frame = g_sceneFrame;
    eyeFrame[0] = g_eyeFrame[0]; eyeFrame[1] = g_eyeFrame[1];
    return true;
}

static bool IsBackBuffer(IDirect3DDevice9* dev, IDirect3DSurface9* s) {
    if (!s) return false;
    IDirect3DSurface9* bb = GameBackBuffer(dev);
    bool r = bb == s;
    if (bb) bb->Release();
    return r;
}

// A camera draw into a shared (untwinned, e.g. square) target with the main scene's projection: a planar reflection
// or similar view of the scene (Arkham Asylum's floors). Drawn with the game camera it stays head-locked and ghosts
// against the head-tracked scene; drawn with the left eye's camera it follows the head (right eye: small IPD error).
// Shadow-map / light views have their own projection and keep the game's camera.
static unsigned g_sharedHead = 0;
static bool SharedTargetFollowsHead() {
    if (!G().s.sharedTargetHead || g_cur.vp < 0 || g_gameP00 <= 0 || g_gameP11 <= 0) return false;
    EyeView* ev = GetView(Load(g_cur.vp));
    if (!ev || ev->ui) return false;
    float p00 = ev->P.m[0][0] / ev->P.m[2][3], p11 = ev->P.m[1][1] / ev->P.m[2][3];
    return fabsf(p00 / g_gameP00 - 1) < 0.01f && fabsf(p11 / g_gameP11 - 1) < 0.01f;
}

int StereoPreDraw(IDirect3DDevice9* dev) {
    g_didPatch = false;
    g_noPatch = false;
    if (G().s.dbgHideShadows && g_curPS.s2s >= 0) return SD_SKIP;   // debug: no modulated shadows
    if (!g_active || !Native()) return SD_NONE;
    if (g_rt0IsBB) {
        // the gamma composite of the scene targets: the eye images (only when this frame had a 3D scene)
        // UE3's gamma pass (InverseGamma + SceneColorTexture), or generically (later UE3: uber post-process into the back
        // buffer) any draw that samples a right-eye-twinned scene texture after this frame's 3D scene
        bool samplesTwin = false;
        for (int i = 0; i < 16 && !samplesTwin; ++i) samplesTwin = g_texTwin[i] != nullptr;
        if ((g_curPS.gamma && g_texTwin[0]) || (samplesTwin && G().s.compositeAnyTwin))
            if (g_patched > 0 || g_sceneCopyThisFrame) return SD_COMPOSITE;
        return g_sceneCopyThisFrame && g_hudSurf ? SD_HUD : SD_NONE;
    }
    if (!g_rtTwin[0]) {                          // shared (eye-independent) target, e.g. a shadow map
        if (!g_sceneCopyThisFrame && SharedTargetFollowsHead()) {
            Patch(dev, 0);
            if (g_didPatch) {
                if (g_sharedHead++ == 0) LOGI("stereo: camera draws into a shared target with the scene's projection (reflection?) -> left-eye camera [shared_target_head]");
            }
        }
        return SD_NONE;
    }
    if (g_sceneCopyThisFrame) {   // UI scene (minimap): game camera; both eyes, or (HUD layer) its twin = the HUD matte
        g_noPatch = true;
        if (HudLayer() && g_frameStereo) FillTwinForHud(dev);
        return SD_NATIVE;
    }
    Patch(dev, 0);
    if (g_didPatch) g_patched++;
    return SD_NATIVE;
}

void StereoPostDraw(IDirect3DDevice9* dev, int) {
    if (!g_didPatch) return;
    g_didPatch = false;
    InternalScope in;   // put the game's values back (it may not set them again for the next draw)
    for (int i = 0; i < g_nVS; ++i) dev->SetVertexShaderConstantF(g_patchedRegsVS[i], Sh().vsc[g_patchedRegsVS[i]], g_patchedCntVS[i]);
    for (int i = 0; i < g_nPS; ++i) dev->SetPixelShaderConstantF(g_patchedRegsPS[i], Sh().psc[g_patchedRegsPS[i]], g_patchedCntPS[i]);
    g_nVS = g_nPS = 0;
}

// ---- native: bind the right-eye twins of everything bound (targets, depth, sampled textures)
static void BindTargets(IDirect3DDevice9* dev, bool right) {
    for (int i = 0; i < 4; ++i) {
        if (!g_rt[i] || (i > 0 && !g_rtTwin[i])) continue;   // MRT slots without a twin stay as they are
        IDirect3DSurface9* s = right && g_rtTwin[i] ? g_rtTwin[i] : g_rt[i];
        dev->SetRenderTarget(i, s);
    }
    if (g_dsTwin) dev->SetDepthStencilSurface(right ? g_dsTwin : g_ds);
    ApplyViewport(dev, g_rt[0]);   // twins have the same size as the game's target
}

static void BindTextures(IDirect3DDevice9* dev, bool right) {
    for (int i = 0; i < 20; ++i)
        if (g_texTwin[i]) dev->SetTexture(StageId(i), right ? g_texTwin[i] : g_tex[i]);
}

// ---- native, batched (stereobatch.cpp): what the per-draw path does, as data
void StereoBindPassTargets(IDirect3DDevice9* dev, bool right) { BindTargets(dev, right); }
IDirect3DBaseTexture9* StereoTwinTexture(IDirect3DBaseTexture9* t) { return Native() ? Twin(t) : nullptr; }
void StereoNoteRightDraws(unsigned n) { g_rightDraws += n; }
IDirect3DSurface9* StereoBoundRT(DWORD i) { return i < 4 ? g_rt[i] : nullptr; }
IDirect3DSurface9* StereoBoundDS() { return g_ds; }
bool StereoClearWouldMirror() { return g_active && Native() && (g_rtTwin[0] || g_dsTwin); }

// The right eye's constants for the current draw (call between the game's draw and StereoPostDraw): VS and PS.
int StereoRightPatches(StereoPatchReg out[8]) {
    if (g_noPatch || !g_didPatch) return 0;
    // compute the right eye's values by patching, reading them back from our upload record
    int n = 0;
    bool needView = g_cur.vp >= 0 || g_haveViewVP;
    if (!needView) return 0;
    EyeView* ev = GetView(g_cur.vp >= 0 ? Load(g_cur.vp) : g_viewVP);
    if (!ev) return 0;
    auto add = [&](bool ps, int reg, int count, const float* right) {
        if (n >= 8) return;
        StereoPatchReg& p = out[n++];
        p.ps = ps; p.reg = reg; p.count = count;
        memcpy(p.right, right, count * 16);
        memcpy(p.game, ps ? Sh().psc[reg] : Sh().vsc[reg], count * 16);
    };
    if (g_cur.vp >= 0) add(false, g_cur.vp, 4, &ev->VPe[1].m[0][0]);
    if (g_cur.cam >= 0) { float c[4] = {ev->camE[1][0], ev->camE[1][1], ev->camE[1][2], Sh().vsc[g_cur.cam][3]}; add(false, g_cur.cam, 1, c); }
    if (g_cur.camw >= 0) { float c[4] = {ev->camE[1][0], ev->camE[1][1], ev->camE[1][2], Sh().vsc[g_cur.camw][3]}; add(false, g_cur.camw, 1, c); }
    if (g_cur.s2w >= 0) add(false, g_cur.s2w, g_cur.s2wN, &ev->QiVPe[1].m[0][0]);
    if (g_curPS.s2s >= 0 && G().s.dbgShadowPatch) { M4 Me = Mul(ev->QiVPe[1], Mul(ev->invQiVP, LoadPS(g_curPS.s2s))); add(true, g_curPS.s2s, 4, &Me.m[0][0]); }
    if (g_curPS.s2w >= 0) add(true, g_curPS.s2w, g_curPS.s2wN, &ev->QiVPe[1].m[0][0]);
    if (g_curPS.cam >= 0) { float c[4] = {ev->camE[1][0], ev->camE[1][1], ev->camE[1][2], Sh().psc[g_curPS.cam][3]}; add(true, g_curPS.cam, 1, c); }
    if (g_cur.ivp >= 0) add(false, g_cur.ivp, 4, &ev->iVPe[1].m[0][0]);
    if (g_curPS.vp >= 0) add(true, g_curPS.vp, 4, &ev->VPe[1].m[0][0]);
    if (g_curPS.ivp >= 0) add(true, g_curPS.ivp, 4, &ev->iVPe[1].m[0][0]);
    return n;
}

bool StereoRightBegin(IDirect3DDevice9* dev) {
    static bool warned = false;
    if (!warned && ((g_ds && !g_dsTwin) || (g_rt[1] && !g_rtTwin[1]))) {
        warned = true;
        LOGW("stereo: a stereo draw has a shared depth buffer or MRT target - the right eye may overwrite the left");
    }
    InternalScope in;
    if (!G().s.debugNativeNoSwitch) {
        BindTargets(dev, true);
        BindTextures(dev, true);
    }
    bool keep = g_didPatch;
    Patch(dev, 1);
    g_didPatch = keep || g_didPatch;
    g_rightDraws++;
    return true;
}

void StereoRightEnd(IDirect3DDevice9* dev) {
    if (G().s.debugNativeNoSwitch) return;
    InternalScope in;
    BindTargets(dev, false);
    BindTextures(dev, false);
}

void StereoAfterClear(IDirect3DDevice9* dev, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR c, float z, DWORD s) {
    if (!g_active || !Native() || (!g_rtTwin[0] && !g_dsTwin)) return;
    InternalScope in;
    BindTargets(dev, true);
    dev->Clear(count, rects, flags, c, z, s);
    BindTargets(dev, false);
    g_mirrored++;
}

void StereoAfterStretchRect(IDirect3DDevice9* dev, IDirect3DSurface9* src, const RECT* sr, IDirect3DSurface9* dst, const RECT* dr,
                            D3DTEXTUREFILTERTYPE f) {
    if (!g_active || !Native() || !src || !dst) return;
    IDirect3DSurface9* dt = Twin(dst);
    if (!dt) return;
    IDirect3DSurface9* st = Twin(src);
    InternalScope in;
    dev->StretchRect(st ? st : src, sr, dt, dr, f);
    g_mirrored++;
}

// ---- HUD: UI draws are repeated into g_hudSurf (cleared transparent) with alpha accumulated as
// a_dst = a_src + a_dst * (1 - a_src), so the result is premultiplied over transparent black.
static DWORD RsOr(D3DRENDERSTATETYPE s, DWORD def) { return Sh().rsKnown[s] ? Sh().rs[s] : def; }

bool StereoHudBegin(IDirect3DDevice9* dev) {
    if (!g_hudSurf) return false;
    InternalScope in;
    dev->SetRenderTarget(0, g_hudSurf);
    ApplyViewport(dev, g_rt[0]);   // the HUD texture has the UI target's (possibly scaled) size: same pixel grid as its stencil
    if (RsOr(D3DRS_ALPHABLENDENABLE, FALSE)) {
        dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
        dev->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
        dev->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
        dev->SetRenderState(D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
    }
    g_hudDraws++;
    return true;
}

void StereoHudEnd(IDirect3DDevice9* dev) {
    InternalScope in;
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, RsOr(D3DRS_SEPARATEALPHABLENDENABLE, FALSE));
    dev->SetRenderState(D3DRS_SRCBLENDALPHA, RsOr(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE));
    dev->SetRenderState(D3DRS_DESTBLENDALPHA, RsOr(D3DRS_DESTBLENDALPHA, D3DBLEND_ZERO));
    dev->SetRenderState(D3DRS_BLENDOPALPHA, RsOr(D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD));
    dev->SetRenderTarget(0, g_rt[0]);
    ApplyViewport(dev, g_rt[0]);
}

void StereoOnStretchRect(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*) {}

// ---- HUD layer for UE3 UI scenes (minimap): after the first composite the right-eye twins are free. UI-scene draws
// render into the twins filled black (the game's own targets keep the desktop picture), the next composite draws
// the twins into g_hudTmp, and a tiny pixel shader writes it into the HUD texture with alpha = brightest channel
// (premultiplied; exact for the glowing minimap lines on a transparent background).
static IDirect3DTexture9* g_hudTmpTex = nullptr;
static IDirect3DSurface9* g_hudTmp = nullptr;
static IDirect3DPixelShader9* g_psMaxAlpha = nullptr;
static bool g_psTried = false;

static bool EnsureHudPass(IDirect3DDevice9* dev) {
    if (!g_hudTmp) {
        if (FAILED(dev->CreateTexture(g_hudW, g_hudH, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_hudTmpTex, nullptr)) ||
            FAILED(g_hudTmpTex->GetSurfaceLevel(0, &g_hudTmp))) {
            LOGE("stereo: HUD temp texture failed");
            return false;
        }
    }
    if (!g_psMaxAlpha && !g_psTried) {
        g_psTried = true;
        // D3DX of the game (d3dx9_35, loaded by TLR.exe) assembles the shader
        HMODULE x = nullptr;
        struct Buf { virtual HRESULT __stdcall QI(REFIID, void**) = 0; virtual ULONG __stdcall AddRef() = 0; virtual ULONG __stdcall Release() = 0;
                     virtual void* __stdcall Ptr() = 0; virtual DWORD __stdcall Size() = 0; };
        typedef HRESULT(WINAPI * Asm)(const char*, UINT, const void*, void*, DWORD, Buf**, Buf**);
        Asm f = nullptr;
        auto loadD3dx = [&] {
            x = GetModuleHandleW(L"d3dx9_35.dll");
            if (!x) x = LoadLibraryW(L"d3dx9_35.dll");
            f = x ? (Asm)GetProcAddress(x, "D3DXAssembleShader") : nullptr;
        };
        static const char src[] =
            "ps_2_0\n"
            "dcl t0.xy\n"
            "dcl_2d s0\n"
            "texld r0, t0, s0\n"
            "max r1.x, r0.x, r0.y\n"
            "max r1.x, r1.x, r0.z\n"
            "mul_sat r0.w, r1.x, c0.x\n"
            "mov oC0, r0\n";
        // the same program as bytecode (no D3DX needed): tried first
        static const DWORD kBytecode[] = {
            0xFFFF0200,                                      // ps_2_0
            0x0200001F, 0x80000000, 0xB0030000,              // dcl t0.xy
            0x0200001F, 0x90000000, 0xA00F0800,              // dcl_2d s0
            0x03000042, 0x800F0000, 0xB0E40000, 0xA0E40800,  // texld r0, t0, s0
            0x0300000B, 0x80010001, 0x80000000, 0x80550000,  // max r1.x, r0.x, r0.y
            0x0300000B, 0x80010001, 0x80000001, 0x80AA0000,  // max r1.x, r1.x, r0.z
            0x03000005, 0x80180000, 0x80000001, 0xA0000000,  // mul_sat r0.w, r1.x, c0.x
            0x02000001, 0x800F0800, 0x80E40000,              // mov oC0, r0
            0x0000FFFF};
        if (SUCCEEDED(dev->CreatePixelShader(kBytecode, &g_psMaxAlpha)) && g_psMaxAlpha) {
            LOGI("stereo: HUD alpha shader ready (built-in bytecode)");
            return true;
        }
        LOGW("stereo: built-in HUD alpha shader rejected, trying D3DX");
        loadD3dx();
        Buf *code = nullptr, *err = nullptr;
        HRESULT hr = f ? f(src, (UINT)sizeof(src) - 1, nullptr, nullptr, 0, &code, &err) : E_FAIL;
        if (SUCCEEDED(hr) && code) hr = dev->CreatePixelShader((const DWORD*)code->Ptr(), &g_psMaxAlpha);
        if (FAILED(hr)) LOGE("stereo: HUD alpha shader failed %08lX (%s)", hr, err ? (const char*)err->Ptr() : f ? "" : "no D3DXAssembleShader");
        else LOGI("stereo: HUD alpha shader ready");
        if (code) code->Release();
        if (err) err->Release();
    }
    return g_psMaxAlpha != nullptr;
}

static void ReleaseHudPass() {
    if (g_hudTmp) g_hudTmp->Release();
    if (g_hudTmpTex) g_hudTmpTex->Release();
    g_hudTmp = nullptr; g_hudTmpTex = nullptr;
    // the shader survives a reset (not a pool resource)
}

// g_hudTmp -> g_hudSurf with alpha from colour (premultiplied over what the HUD already has)
static void HudFromTmp(IDirect3DDevice9* dev) {
    IDirect3DStateBlock9* sb = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb))) return;
    dev->SetRenderTarget(0, g_hudSurf);
    for (DWORD i = 1; i < 4; ++i) dev->SetRenderTarget(i, nullptr);
    dev->SetDepthStencilSurface(nullptr);
    D3DVIEWPORT9 vp = {0, 0, g_hudW, g_hudH, 0, 1};
    dev->SetViewport(&vp);
    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(g_psMaxAlpha);
    float gain[4] = {G().s.hudAlphaGain, 0, 0, 0};
    dev->SetPixelShaderConstantF(0, gain, 1);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    dev->SetTexture(0, g_hudTmpTex);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 15);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    float w = (float)g_hudW, h = (float)g_hudH;
    struct V { float x, y, z, rhw, u, v; } q[4] = {{-0.5f, -0.5f, 0, 1, 0, 0}, {w - 0.5f, -0.5f, 0, 1, 1, 0}, {-0.5f, h - 0.5f, 0, 1, 0, 1}, {w - 0.5f, h - 0.5f, 0, 1, 1, 1}};
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
    sb->Apply();
    sb->Release();
}

// UI scene draw into a twinned target: in HUD-layer mode its twin starts black (once per frame per target).
static void FillTwinForHud(IDirect3DDevice9* dev) {
    IDirect3DSurface9* t = g_rtTwin[0];
    if (!t) return;
    for (int i = 0; i < g_filledN; ++i) if (g_filled[i] == t) return;
    if (g_filledN < 8) g_filled[g_filledN++] = t;
    InternalScope in;
    dev->ColorFill(t, nullptr, D3DCOLOR_ARGB(0, 0, 0, 0));
}

// The gamma composite of the scene (UE3: full-screen draw into the back buffer with InverseGamma/SceneColorTexture).
// The game's draw went into the back buffer (desktop window). Here it is drawn again into both eye textures at the
// scene resolution: left with the game's textures, right with the twins. From now on draws into the back buffer
// are the canvas UI (HUD), draws into the scene targets are UI scenes (both eyes, game camera).
static D3DVIEWPORT9 FullVp(UINT w, UINT h) { return {0, 0, w, h, 0.0f, 1.0f}; }
void StereoComposite(IDirect3DDevice9* dev, const std::function<void()>& draw) {
    bool first = !g_sceneCopyThisFrame;
    g_sceneCopyThisFrame = true;
    g_composites++;
    if (!g_eyeSurf[0] || !g_eyeSurf[1]) return;
    // no real 3D scene this frame (menus; the title screen's backdrop is a 3D scene of 2-3 draws): flat screen
    if (first && g_patched < (unsigned)G().s.stereoMinDraws) return;
    // tweak ui_in_scene_flat: a menu drawn into the 3D scene (UI-scene camera before the composite) would be
    // head-locked inside the eye images and hard to read: show such frames on the flat screen (Alpha Protocol's title)
    if (first && G().s.uiInSceneFlat && g_uiInScene > 0) return;
    // a 3D scene after flat frames: this frame was drawn with the game's camera (it stays on the flat screen), the
    // next one gets the eye cameras. A frame that ends up flat is never head-tracked (the title screen moved with
    // the head when its backdrop was patched but the frame was shown flat).
    if (first && !g_patchAllowed) { g_wantStereo = true; return; }
    if (!first && !g_frameStereo) return;   // later composites only matter in a stereo frame
    if (first && g_candP00 > 0) {
        // a real 3D scene (not the title's 3-draw backdrop): its camera is what the culling-FOV search may look for
        g_gameP00 = g_candP00; g_gameP11 = g_candP11; g_gamePFrame = G().frame;
        g_gameFov = {-atanf(1 / g_gameP00), atanf(1 / g_gameP00), atanf(1 / g_gameP11), -atanf(1 / g_gameP11)};
    }
    InternalScope in;
    if (!first && HudLayer() && g_frameStereo) {
        // a UI scene was composited: its twins hold it over black -> HUD texture (the eye images stay scene-only)
        if (!EnsureHudPass(dev)) return;
        D3DVIEWPORT9 hv = FullVp(g_hudW, g_hudH);
        RECT hs = {0, 0, (LONG)g_hudW, (LONG)g_hudH};
        dev->SetRenderTarget(0, g_hudTmp);
        dev->SetViewport(&hv);
        dev->SetScissorRect(&hs);
        BindTextures(dev, true);
        draw();
        BindTextures(dev, false);
        HudFromTmp(dev);
        dev->SetRenderTarget(0, g_rt[0]);
        ApplyViewport(dev, g_rt[0]);
        g_hudScenes++;
        return;
    }
    D3DVIEWPORT9 vp = FullVp(g_texW, g_texH);
    RECT sc = {0, 0, (LONG)g_texW, (LONG)g_texH};
    for (int e = 0; e < 2; ++e) {
        dev->SetRenderTarget(0, g_eyeSurf[e]);
        dev->SetViewport(&vp);
        dev->SetScissorRect(&sc);
        BindTextures(dev, e == 1);
        draw();
    }
    BindTextures(dev, false);
    dev->SetRenderTarget(0, g_rt[0]);
    ApplyViewport(dev, g_rt[0]);
    for (int e = 0; e < 2; ++e) {
        g_eyeValid[e] = true;
        g_eyePose[e] = g_framePose[e];
        g_eyeFov[e] = G().s.fovMode == 1 ? g_frameFov[e] : g_gameFov;
        g_eyeTime[e] = g_frameTime;
    }
    memcpy(g_sceneRot, g_camRot, sizeof(g_camRot));
    g_sceneFrame = G().frame;
    g_eyeFrame[0] = g_eyeFrame[1] = G().frame;
    g_frameStereo = true;
    dev->ColorFill(g_hudSurf, nullptr, D3DCOLOR_ARGB(0, 0, 0, 0));
    g_uiPhase = true;
    static int logged = 0;
    if (logged++ < 2) LOGI("stereo: scene composite #%u at frame %ld (%u patched draws so far) -> eye images %ux%u", g_composites, G().frame, g_patched, g_texW, g_texH);
}

void StereoLastFrame(bool* stereo, unsigned* patched) { *stereo = g_lastStereo; *patched = g_lastPatched; }
bool StereoFrameReady() { return g_frameStereo && g_eyeValid[0] && g_eyeValid[1]; }
IDirect3DSurface9* StereoEyeSurface(int eye) { return g_eyeSurf[eye & 1]; }
IDirect3DSurface9* StereoHudSurface() { return g_hudSurf; }
void StereoPoses(xrb::IpcPose pose[2], xrb::IpcFov fov[2], int64_t* time) {
    for (int e = 0; e < 2; ++e) { pose[e] = g_eyePose[e]; fov[e] = g_eyeFov[e]; }
    *time = g_eyeTime[0] > g_eyeTime[1] ? g_eyeTime[0] : g_eyeTime[1];
}

void StereoLogStatus() {
    if (!G().s.stereo) return;
    double n = g_statFrames ? g_statFrames : 1;
    LOGI("stereo (%s): %u of %u frames in stereo, per frame %.0f patched, %.0f right-eye draws, %.0f mirrored clears/copies, "
         "%.0f HUD draws, %.1f composites, %u UI scenes to the HUD; %u twins",
         Native() ? "native" : "aer", g_statStereo, g_statFrames, g_statPatched / n, g_statRight / n, g_statMirror / n, g_statHud / n,
         g_statComp / n, g_hudScenes, g_twins);
    g_hudScenes = 0;
    g_statFrames = g_statStereo = g_statPatched = g_statHud = g_statRight = g_statMirror = g_statComp = 0;
}


// ------------------------------------------------------------------ D3D11 backend (see stereo.h)
static volatile LONG g_eyeGen11 = 1;
unsigned StereoEyeGen11() { return (unsigned)g_eyeGen11; }
bool StereoEyesBegin11() {
    EyeLockGuard lk;
    InvalidateViews();
    struct GenBump { ~GenBump() { InterlockedIncrement(&g_eyeGen11); } } bump_;   // after the new matrices are in place
    xrb::IpcPose pose[2];
    xrb::IpcFov fov[2];
    int64_t t = 0;
    if (!xrb::ClientGetLatestPoses(pose, fov, &t)) { g_EprevOk = false; return false; }
    // keep last frame's eye transforms: the game's previous-frame camera must be combined with the head pose that frame
    // was rendered with. With this frame's pose instead, TAA / motion vectors never see head motion and history
    // pixels stay fixed on screen (user: "a big portion of the ground moves with the HMD").
    static bool have = false;
    if (have) { g_Eprev[0] = g_E[0]; g_Eprev[1] = g_E[1]; memcpy(g_prevFov, g_frameFov, sizeof(g_prevFov)); g_EprevOk = true; }
    memcpy(g_framePose, pose, sizeof(pose));
    memcpy(g_frameFov, fov, sizeof(fov));
    g_frameTime = t;
    // engine head (D3D11): this was only done in the D3D9 path, so "Game camera follows head" turned the camera with a
    // stale identity pose and the eyes then removed the head rotation (test 047: "no pitch").
    g_headPose = pose[0];
    g_headPose.px = 0.5f * (pose[0].px + pose[1].px); g_headPose.py = 0.5f * (pose[0].py + pose[1].py); g_headPose.pz = 0.5f * (pose[0].pz + pose[1].pz);
    g_engineHeadActive = G().s.engineHead && G().engineHeadHooked && G().frame - g_engineHeadFrame <= 2;
    BuildEye(0, pose[0], pose[1]);
    BuildEye(1, pose[1], pose[0]);
    if (!have) { g_Eprev[0] = g_E[0]; g_Eprev[1] = g_E[1]; memcpy(g_prevFov, g_frameFov, sizeof(g_prevFov)); g_EprevOk = true; have = true; }
    return true;
}
bool StereoEyeMats11Locked(const float VPf[16], EyeMats11* o);
bool StereoEyeMats11Prev(const float VPf[16], EyeMats11* o) {
    EyeLockGuard lk;
    if (!g_EprevOk || !G().s.prevFramePose) return StereoEyeMats11Locked(VPf, o);
    M4 VP;
    memcpy(VP.m, VPf, sizeof(VP.m));
    EyeView ev{};
    if (!Decompose(VP, ev, g_Eprev, g_prevFov) || !ev.persp) return false;
    for (int e = 0; e < 2; ++e) {
        memcpy(o->VPe[e], ev.VPe[e].m, 64);
        memcpy(o->iVPe[e], ev.iVPe[e].m, 64);
        memcpy(o->QiVPe[e], ev.QiVPe[e].m, 64);
        memcpy(o->camE[e], ev.camE[e], 12);
        memcpy(o->Ve[e], ev.Ve[e].m, 64);
        memcpy(o->Pe[e], ev.Pe[e].m, 64);
        memcpy(o->iVe[e], ev.iVe[e].m, 64);
    }
    memcpy(o->invQiVP, ev.invQiVP.m, 64);
    o->P00 = ev.P.m[0][0] / ev.P.m[2][3];
    o->P11 = ev.P.m[1][1] / ev.P.m[2][3];
    return true;
}
bool StereoEyeMats11(const float VPf[16], EyeMats11* o) { EyeLockGuard lk; return StereoEyeMats11Locked(VPf, o); }
bool StereoEyeMats11Locked(const float VPf[16], EyeMats11* o) {
    M4 VP;
    memcpy(VP.m, VPf, sizeof(VP.m));
    EyeView* ev = GetView(VP);
    if (!ev) return false;
    for (int e = 0; e < 2; ++e) {
        memcpy(o->VPe[e], ev->VPe[e].m, 64);
        memcpy(o->iVPe[e], ev->iVPe[e].m, 64);
        memcpy(o->QiVPe[e], ev->QiVPe[e].m, 64);
        memcpy(o->camE[e], ev->camE[e], 12);
        memcpy(o->Ve[e], ev->Ve[e].m, 64);
        memcpy(o->Pe[e], ev->Pe[e].m, 64);
        memcpy(o->iVe[e], ev->iVe[e].m, 64);
    }
    memcpy(o->invQiVP, ev->invQiVP.m, 64);
    o->P00 = ev->P.m[0][0] / ev->P.m[2][3];
    o->P11 = ev->P.m[1][1] / ev->P.m[2][3];
    return true;
}
void StereoEyesCommit11(float p00, float p11) {
    EyeLockGuard lk;
    if (p00 > 0 && p11 > 0) {
        g_gameP00 = p00; g_gameP11 = p11; g_gamePFrame = G().frame;
        g_gameFov = {-atanf(1 / p00), atanf(1 / p00), atanf(1 / p11), -atanf(1 / p11)};
    }
    for (int e = 0; e < 2; ++e) {
        g_eyeValid[e] = true;
        g_eyePose[e] = g_framePose[e];
        g_eyeFov[e] = G().s.fovMode == 1 ? g_frameFov[e] : g_gameFov;
        g_eyeTime[e] = g_frameTime;
    }
}

} // namespace fvr

// Row-vector head transform for the game camera (v_world = v_local * H * World): head rotation + head position (world
// units). False when the engine head mode is off or there is no tracked pose this frame.
namespace fvr {
void StereoEngineHeadApplied() { g_engineHeadFrame = G().frame; }
// Head rotation for the culling camera (row vectors, game view space -> head-turned view space), = rotation part of the
// eye transform (eye view = game view * E). testYawDeg / testPitchDeg add a fixed turn (experiments).
bool StereoCullRotation11(float out[16], bool withHead, float testYawDeg, float testPitchDeg) {
    M4 E = Identity();
    {
        EyeLockGuard lk;
        if (withHead && g_frameTime) { E = g_E[0]; E.m[3][0] = E.m[3][1] = E.m[3][2] = 0; }
    }
    if (testYawDeg != 0 || testPitchDeg != 0) {
        float y = testYawDeg * 0.0174533f, p = testPitchDeg * 0.0174533f;
        M4 Ry = Identity(), Rx = Identity();
        Ry.m[0][0] = cosf(y); Ry.m[0][2] = -sinf(y); Ry.m[2][0] = sinf(y); Ry.m[2][2] = cosf(y);
        Rx.m[1][1] = cosf(p); Rx.m[1][2] = sinf(p); Rx.m[2][1] = -sinf(p); Rx.m[2][2] = cosf(p);
        E = Mul(Mul(E, Ry), Rx);
    }
    memcpy(out, E.m, 64);
    return true;
}
bool StereoHeadsetTans(float* vMax, float* hMax) {
    EyeLockGuard lk;
    if (!g_frameTime) return false;
    float v = 0, h = 0;
    for (int e = 0; e < 2; ++e) {
        v = fmaxf(v, fmaxf(tanf(g_frameFov[e].up), -tanf(g_frameFov[e].down)));
        h = fmaxf(h, fmaxf(tanf(g_frameFov[e].right), -tanf(g_frameFov[e].left)));
    }
    *vMax = v; *hMax = h; return v > 0.1f && h > 0.1f;
}
bool StereoEngineHead(float H[16]) {
    if (!G().s.engineHead || !G().engineHeadHooked || !g_frameTime) return false;
    xrb::IpcPose h;
    { EyeLockGuard lk; h = g_headPose; }
    float x = h.qx, y = h.qy, z = h.qz, w = h.qw;
    float Rm[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                      {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                      {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
    for (int i = 0; i < 3; ++i) { for (int j = 0; j < 3; ++j) H[i * 4 + j] = Rm[j][i]; H[i * 4 + 3] = 0; }
    float sc = G().s.headPosition ? G().s.worldScale : 0.f;
    H[12] = h.px * sc; H[13] = h.py * sc; H[14] = h.pz * sc; H[15] = 1;
    return true;
}
}
