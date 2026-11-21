// UEVR3 D3D11 backend - stage 1: RECON (observation only, the game renders unchanged).
//
// Later UE3 games (2011+) also render with D3D11 (BioShock Infinite only; Arkham Origins, Life Is Strange, Styx, Ethan
// Carter optionally) and still import d3d9.dll, so this same proxy DLL is loaded into them. Their shaders carry no
// reflection data (the cooked caches keep only ISGN/OSGN/SHEX chunks), so the camera constants are found by VALUE in
// the constant buffers (cbmatch.cpp) instead of by name as in D3D9.
//
// What this stage does: hooks D3D11CreateDevice(AndSwapChain) and DXGI swap-chain creation, keeps CPU shadow copies
// of every constant buffer the game writes (UpdateSubresource / Map+Unmap), and for each draw looks for the view-
// projection matrix in the bound vertex-shader constant buffers. Every 10 s it logs what it found (per shader: which
// buffer slot / float4 offset holds ViewProjectionMatrix, CameraPosition, ScreenToWorld; camera FOV), and the frame
// structure (render targets, draws). That is the data the stereo stage needs, gathered on the first real run.
#include "d3d11hooks.h"
#include "probe11.h"
#include <functional>
#include <unordered_set>
#include "greedfall.h"
#include "xr_inproc.h"
#include <array>
#include <d3dcommon.h>
#include "globals.h"
#include "hookutil.h"
#include "cbmatch.h"
#include "vtbl11.h"
#include "MinHook.h"
#include "xrbridge/client.h"
#include "d3d9hooks.h"
#include "stereo.h"
#include "menu.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <algorithm>
#include <string>
#include <cstdio>

namespace fvr {

namespace {

#define ORIG11(name, ret, ...) typedef ret(STDMETHODCALLTYPE* name##_t)(__VA_ARGS__); name##_t o_##name = nullptr;
ORIG11(D11CreateBuffer, HRESULT, ID3D11Device*, const D3D11_BUFFER_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**)
ORIG11(D11CreateVertexShader, HRESULT, ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11VertexShader**)
ORIG11(D11CreatePixelShader, HRESULT, ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**)
ORIG11(D11VSSetConstantBuffers, void, ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*)
ORIG11(D11HSSetConstantBuffers, void, ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*)
ORIG11(D11DSSetConstantBuffers, void, ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*)
ORIG11(D11GSSetConstantBuffers, void, ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*)
ORIG11(D11PSSetConstantBuffers, void, ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*)
ORIG11(D11VSSetShader, void, ID3D11DeviceContext*, ID3D11VertexShader*, ID3D11ClassInstance* const*, UINT)
ORIG11(D11PSSetShader, void, ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT)
ORIG11(D11Draw, void, ID3D11DeviceContext*, UINT, UINT)
ORIG11(D11DrawIndexed, void, ID3D11DeviceContext*, UINT, UINT, INT)
ORIG11(D11DrawInstanced, void, ID3D11DeviceContext*, UINT, UINT, UINT, UINT)
ORIG11(D11DrawIndexedInstanced, void, ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT)
ORIG11(D11Map, HRESULT, ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*)
ORIG11(D11Unmap, void, ID3D11DeviceContext*, ID3D11Resource*, UINT)
ORIG11(D11UpdateSubresource, void, ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT)
ORIG11(D11OMSetRenderTargets, void, ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*)
ORIG11(D11CreateTexture2D, HRESULT, ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**)
ORIG11(D11CreateRenderTargetView, HRESULT, ID3D11Device*, ID3D11Resource*, const D3D11_RENDER_TARGET_VIEW_DESC*, ID3D11RenderTargetView**)
ORIG11(D11CreateShaderResourceView, HRESULT, ID3D11Device*, ID3D11Resource*, const D3D11_SHADER_RESOURCE_VIEW_DESC*, ID3D11ShaderResourceView**)
ORIG11(D11CreateDepthStencilView, HRESULT, ID3D11Device*, ID3D11Resource*, const D3D11_DEPTH_STENCIL_VIEW_DESC*, ID3D11DepthStencilView**)
ORIG11(D11PSSetShaderResources, void, ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*)
ORIG11(D11ClearRenderTargetView, void, ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT*)
ORIG11(D11ClearDepthStencilView, void, ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8)
ORIG11(D11CopyResource, void, ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*)
ORIG11(D11CopySubresourceRegion, void, ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*)
ORIG11(D11ResolveSubresource, void, ID3D11DeviceContext*, ID3D11Resource*, UINT, ID3D11Resource*, UINT, DXGI_FORMAT)
ORIG11(D11OMSetRenderTargetsAndUnorderedAccessViews, void, ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*,
       UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*)
ORIG11(D11Dispatch, void, ID3D11DeviceContext*, UINT, UINT, UINT)
ORIG11(D11CreateUnorderedAccessView, HRESULT, ID3D11Device*, ID3D11Resource*, const D3D11_UNORDERED_ACCESS_VIEW_DESC*, ID3D11UnorderedAccessView**)
ORIG11(D11CSSetShaderResources, void, ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*)
ORIG11(D11CSSetUnorderedAccessViews, void, ID3D11DeviceContext*, UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*)
ORIG11(D11DispatchIndirect, void, ID3D11DeviceContext*, ID3D11Buffer*, UINT)
ORIG11(D11DrawAuto, void, ID3D11DeviceContext*)
ORIG11(D11DrawInstancedIndirect, void, ID3D11DeviceContext*, ID3D11Buffer*, UINT)
ORIG11(D11DrawIndexedInstancedIndirect, void, ID3D11DeviceContext*, ID3D11Buffer*, UINT)
ORIG11(D11VSSetShaderResources, void, ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*)
ORIG11(D11CreateDeferredContext, HRESULT, ID3D11Device*, UINT, ID3D11DeviceContext**)
ORIG11(D11RSSetViewports, void, ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*)
ORIG11(D11ExecuteCommandList, void, ID3D11DeviceContext*, ID3D11CommandList*, BOOL)
ORIG11(D11FinishCommandList, HRESULT, ID3D11DeviceContext*, BOOL, ID3D11CommandList**)
ORIG11(DxgiPresent, HRESULT, IDXGISwapChain*, UINT, UINT)
ORIG11(DxgiCreateSwapChain, HRESULT, IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**)
ORIG11(DxgiCreateSwapChainForHwnd, HRESULT, IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
       const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**)
typedef HRESULT(WINAPI* CreateDevice_t)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                        ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
typedef HRESULT(WINAPI* CreateDeviceSC_t)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                          const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*,
                                          ID3D11DeviceContext**);
typedef HRESULT(WINAPI* CreateFactory_t)(REFIID, void**);
CreateDevice_t o_D3D11CreateDevice = nullptr;
CreateDeviceSC_t o_D3D11CreateDeviceAndSwapChain = nullptr;
CreateFactory_t o_CreateDXGIFactory = nullptr, o_CreateDXGIFactory1 = nullptr;

// Our own D3D11 work through the hooked vtables (the VR menu's ImGui renderer): straight to the originals, no
// tracking, twins or stereo. Per thread (loading threads keep being tracked meanwhile).
thread_local int g_bypass11 = 0;
struct Bypass11 { Bypass11() { ++g_bypass11; } ~Bypass11() { --g_bypass11; } };

ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_imm = nullptr;
// g_mx is held for microseconds at a time but taken ~3400 times per frame by 16 recording threads: an OS mutex made
// them sleep and wake (test 096: 15 ms waiting vs 1.5 ms held per frame). Spin briefly first (spin_lock, live switch).
bool SpinLockOn();
struct SpinMx {
    volatile LONG v = 0;
    bool try_lock() { return v == 0 && InterlockedCompareExchange(&v, 1, 0) == 0; }
    void lock() {
        if (try_lock()) return;
        int spins = SpinLockOn() ? 4000 : 0;
        for (int i = 0;; ++i) {
            if (try_lock()) return;
            if (i < spins) YieldProcessor();
            else if (i < spins + 50) SwitchToThread();
            else Sleep(0);
        }
    }
    void unlock() { InterlockedExchange(&v, 0); }
};
bool SpinLockOn() { return G().s.spinLock; }
SpinMx g_mx;   // shadow copies may be written from loading threads (CreateBuffer with initial data)
// perf instrumentation (build 19): time threads wait for / hold g_mx, time spent in the mod's hooks, bytes copied
static volatile LONG64 g_pfDraw = 0, g_pfWait = 0, g_pfHeld = 0, g_pfLocks = 0, g_pfHook = 0, g_pfCopied = 0, g_pfSkipped = 0;
static volatile LONG64 g_pfObs = 0, g_pfPatch = 0, g_pfGameL = 0, g_pfGameR = 0, g_pfBind = 0, g_pfObsN = 0, g_pfObsHit = 0, g_pfEmCalc = 0, g_pfEmHit = 0;
static inline LONG64 Qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
struct MxGuard {
    LONG64 t1;
    MxGuard() { LONG64 t0 = Qpc(); g_mx.lock(); t1 = Qpc(); InterlockedAdd64(&g_pfWait, t1 - t0); InterlockedIncrement64(&g_pfLocks); }
    ~MxGuard() { InterlockedAdd64(&g_pfHeld, Qpc() - t1); g_mx.unlock(); }
};
struct AddTimer { volatile LONG64* t; LONG64 t0; explicit AddTimer(volatile LONG64* p) : t(p), t0(Qpc()) {} ~AddTimer() { InterlockedAdd64(t, Qpc() - t0); } };
struct HookTimer { LONG64 t0 = Qpc(); ~HookTimer() { InterlockedAdd64(&g_pfHook, Qpc() - t0); } };

// constant buffers: CPU copy of what the game last wrote
struct Shadow { std::vector<float> f; UINT bytes = 0; void* mapped = nullptr; uint32_t ver = 0; long gen = -1; };
std::unordered_map<ID3D11Buffer*, Shadow> g_cb;   // contents written outside deferred contexts (creation, immediate context)
volatile LONG g_verSeqL = 0;                      // shadow versions are unique across all contexts
inline uint32_t NextVer() { return (uint32_t)InterlockedIncrement(&g_verSeqL); }
volatile LONG g_cbGen = 0;                        // +1 per constant buffer created: per-context lock-free caches revalidate

// ---- per-context state. The immediate context and every deferred context (GreedFall records its scene on worker
//      threads into command lists) keep their own bindings, constant-buffer contents and per-eye buffers; the
//      right-eye draw is recorded into the same context right after the game's draw, so command lists replay both.
const UINT kSrv = 32;
struct Bind11 {
    ID3D11RenderTargetView* rtv[8] = {}; ID3D11RenderTargetView* rtvT[8] = {};
    UINT nrtv = 0;
    ID3D11DepthStencilView* dsv = nullptr; ID3D11DepthStencilView* dsvT = nullptr;
    ID3D11ShaderResourceView* srv[kSrv] = {}; ID3D11ShaderResourceView* srvT[kSrv] = {};
    UINT maxSrv = 0;   // highest bound slot + 1
    int nTwinSrv = 0;
    bool rt0IsBB = false;
};
struct PatchBuf { ID3D11Buffer* b[2] = {}; UINT bytes = 0; uint32_t ver[2] = {}; long frame[2] = {-1, -1}; float key[2][16]; };
const UINT kCs = 16, kUav = 8;
struct DrawCam;
struct Ctx11 {
    bool deferred = false;
    long recFrame = -1; uint32_t recPatched = 0;   // command list being recorded: frame of its first patched draw, patched draws
    ID3D11Buffer* vsCB[14] = {}; ID3D11Buffer* psCB[14] = {};
    ID3D11Buffer* tessCB[3][14] = {};   // hull, domain, geometry shader constant buffers (tessellated terrain projects in the DS)
    ID3D11VertexShader* curVS = nullptr; ID3D11PixelShader* curPS = nullptr;
    Bind11 b;
    ID3D11ShaderResourceView* csSrv[kCs] = {}; ID3D11ShaderResourceView* csSrvT[kCs] = {};
    ID3D11UnorderedAccessView* csUav[kUav] = {}; ID3D11UnorderedAccessView* csUavT[kUav] = {};
    std::unordered_map<ID3D11Buffer*, Shadow> cb;           // deferred: what was written into buffers on this context
    std::unordered_set<ID3D11Resource*> nonCb; long nonCbGen = -1;   // deferred: mapped resources that are no tracked constant buffer
    std::unordered_map<ID3D11Buffer*, PatchBuf> patch;      // per-eye copies of the game's constant buffers
    std::vector<float> scratch;
    // eye_mat_cache: this frame's eye matrices for the last camera seen on this context (no global lock per draw)
    unsigned emGen = 0, em2Gen = 0; bool emOk = false, em2Ok = false, em2Prev = false; float emKey[16] = {}, em2Key[16] = {};
    EyeMats11 em{}, em2{};
    // draw_cam_cache: the previous draw's camera check and what it depended on
    struct DcKey {
        bool valid = false, haveMain = false; long frame = -1; LONG cbGen = -1; size_t cbSize = 0;
        void* vs = nullptr; void* ps = nullptr;
        int n = 0; bool isPs[16]; int slot[16]; ID3D11Buffer* buf[16]; Shadow* sh[16]; uint32_t ver[16];
        bool searched = false;   // a layout search ran during that check: not reused
    } dck;
    DrawCam* dcc = nullptr;   // (allocated on first use)
    ID3D11Buffer* dofCb = nullptr; UINT dofCbBytes = 0;
    ID3D11RenderTargetView* vrsRtv = nullptr; long vrsRtvFrame = -1; bool vrsRtvOk = false;   // VrsTarget cache
    int vrsState = -1;   // VRS on this context: -1 unknown, 0 off, 1 left-eye pattern, 2 right-eye pattern   // game_dof off: the DoF pass's cb1 with the blend zeroed
    uint64_t draws = 0;
    float vpW = 0, vpH = 0;   // viewport 0 (dynamic-resolution check)
    void ResetBindings() {
        memset(vsCB, 0, sizeof vsCB); memset(psCB, 0, sizeof psCB); curVS = nullptr; curPS = nullptr; b = Bind11();
        memset(csSrv, 0, sizeof csSrv); memset(csSrvT, 0, sizeof csSrvT); memset(csUav, 0, sizeof csUav); memset(csUavT, 0, sizeof csUavT);
    }
};
std::mutex g_ctxMx;
std::vector<PatchBuf*> g_deadPatch;   // per-eye buffers of re-registered contexts (released later, never while in flight)
std::unordered_map<ID3D11DeviceContext*, Ctx11*> g_ctx;
thread_local ID3D11DeviceContext* t_lastC = nullptr;
thread_local Ctx11* t_lastX = nullptr;
thread_local uint64_t t_ctxGen = 0;
uint64_t g_ctxGen = 1;   // bumped when contexts are dropped: thread caches re-check
unsigned g_deferredCtx = 0;
Ctx11* X(ID3D11DeviceContext* c) {
    if (!c) return nullptr;
    if (c == t_lastC && t_ctxGen == g_ctxGen) return t_lastX;
    std::lock_guard<std::mutex> l(g_ctxMx);
    Ctx11* x = nullptr;
    auto it = g_ctx.find(c);
    if (it != g_ctx.end()) x = it->second;
    else if (c == g_imm) { x = new Ctx11(); g_ctx[c] = x; }
    t_lastC = c; t_lastX = x; t_ctxGen = g_ctxGen;
    return x;
}
void RegisterDeferred(ID3D11DeviceContext* c) {
    std::lock_guard<std::mutex> l(g_ctxMx);
    auto it = g_ctx.find(c);   // a new context may reuse a released one's address
    if (it != g_ctx.end()) { for (auto& kv : it->second->patch) g_deadPatch.push_back(new PatchBuf(kv.second)); delete it->second; }
    Ctx11* x = new Ctx11();
    x->deferred = true;
    g_ctx[c] = x;
    g_deferredCtx++;
    g_ctxGen++;
}
// constant-buffer contents as seen by context x: its own writes first (deferred), then the global copy
Shadow* FindShadow(Ctx11* x, ID3D11Buffer* b) {
    if (x && x->deferred) { auto it = x->cb.find(b); if (it != x->cb.end()) return &it->second; }
    auto it = g_cb.find(b);
    return it == g_cb.end() ? nullptr : &it->second;
}
// Deferred contexts in GreedFall each have their own vtable (on the heap) with other functions than the immediate
// context: every hooked slot of such a vtable is patched when the context is created, and calls are routed to the
// original function of the vtable the context really has.
struct DefVt { void** vt; void* orig[160]; };
DefVt g_defVt[64];
volatile long g_nDefVt = 0;
std::mutex g_defVtMx;
void* g_immVtbl = nullptr;
void* DefOrig(void** vt, int slot) {
    long n = g_nDefVt;
    for (long i = 0; i < n; ++i) if (g_defVt[i].vt == vt) return g_defVt[i].orig[slot];
    return nullptr;
}
template <class T> inline T OrigFor(ID3D11DeviceContext* c, int slot, T immOrig) {
    void** vt = *(void***)c;
    if ((void*)vt == g_immVtbl) return immOrig;
    void* f = DefOrig(vt, slot);
    return f ? (T)f : immOrig;   // (shared / unhooked slots fall back to the immediate trampoline)
}
#define OC(c, Name) OrigFor((c), VT_D11CTX_##Name, o_D11##Name)

// ---- one-frame trace (capture_at_sec): every draw / copy / clear / dispatch / command list on every context
volatile long g_traceState = 0;   // 0 off, 1 armed (next Present), 2 tracing (2 frames), 3 done
long g_traceFrames = 0;
volatile bool g_captureReq = false;
uint64_t g_stereoTotal = 0;
int g_snapLeft = 0; long g_snapNext = 0;   // F11: headset images of a few frames (and a 3-frame trace)
std::mutex g_traceMx;
std::vector<std::string> g_trace;
std::unordered_map<void*, int> g_traceIds;
int TraceId(void* p) { auto it = g_traceIds.find(p); if (it != g_traceIds.end()) return it->second; int n = (int)g_traceIds.size(); g_traceIds[p] = n; return n; }
std::string DescRes(ID3D11View* v) {
    if (!v) return "-";
    ID3D11Resource* r = nullptr; v->GetResource(&r);
    if (!r) return "?";
    char b[96];
    D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC d{}; ((ID3D11Texture2D*)r)->GetDesc(&d);
        snprintf(b, sizeof b, "t%d:%ux%u/%d%s", TraceId(r), d.Width, d.Height, (int)d.Format, d.ArraySize > 1 ? "[]" : "");
    } else snprintf(b, sizeof b, "r%d(dim%d)", TraceId(r), (int)dim);
    r->Release();
    return b;
}
std::string DescR(ID3D11Resource* r) {
    if (!r) return "-";
    char b[96];
    D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC d{}; ((ID3D11Texture2D*)r)->GetDesc(&d);
        snprintf(b, sizeof b, "t%d:%ux%u/%d", TraceId(r), d.Width, d.Height, (int)d.Format);
    } else snprintf(b, sizeof b, "r%d(dim%d)", TraceId(r), (int)dim);
    return b;
}
template <class... A> void Trace(const char* fmt, A... a) {
    if (g_traceState != 2) return;
    char b[640]; snprintf(b, sizeof b, fmt, a...);
    std::lock_guard<std::mutex> l(g_traceMx);
    if (g_trace.size() < 40000) g_trace.push_back(b);
}

// where context x writes a buffer's new contents (nullptr: not a constant buffer we track)
Shadow* WriteShadow(Ctx11* x, ID3D11Buffer* b) {   // (under g_mx)
    long gen = g_cbGen;
    auto g = g_cb.find(b);
    if (g == g_cb.end()) {
        if (x && x->deferred) { if (x->nonCbGen != gen) { x->nonCb.clear(); x->nonCbGen = gen; } x->nonCb.insert(b); }
        return nullptr;
    }
    if (!x || !x->deferred) return &g->second;
    Shadow& s = x->cb[b];
    if (s.bytes != g->second.bytes) { s.bytes = g->second.bytes; s.f = g->second.f; }
    s.gen = gen;
    return &s;
}
// Lock-free paths for deferred contexts (perf, build 19: 16 recording threads queued on g_mx ~70 ms per frame in busy
// views). A context is used by one thread at a time, so its own maps need no lock while the global set of constant
// buffers has not changed (g_cbGen).
enum { kFastMiss = 0, kFastCb = 1, kFastNotCb = 2 };
int WriteShadowFast(Ctx11* x, ID3D11Buffer* b, Shadow** out) {
    if (!x || !x->deferred || !G().s.lockFree) return kFastMiss;
    long gen = g_cbGen;
    if (x->nonCbGen == gen && x->nonCb.count(b)) return kFastNotCb;
    auto it = x->cb.find(b);
    if (it != x->cb.end() && it->second.gen == gen) { *out = &it->second; return kFastCb; }
    return kFastMiss;
}
// shaders: FNV-1a of the bytecode, and per (shader, slot) what cbmatch found
std::unordered_map<void*, uint32_t> g_vsHash, g_psHash;
constexpr int kCbSlots = 8;   // constant-buffer slots searched for a camera (was 4: a camera in slot 4+ was never found)
struct ShaderInfo { ue3::CbLayout slot[kCbSlots]; int tries = 0, dtries = 0; bool logged = false; float fov = 0; uint64_t draws = 0, camDraws = 0; };
std::unordered_map<uint32_t, ShaderInfo> g_vsInfo, g_psInfo;

struct Stats { uint64_t frames = 0, draws = 0, deferredDraws = 0, camDraws = 0, cbUpdates = 0, cbMaps = 0, rtSets = 0, cmdLists = 0; } g_st, g_last;
DWORD g_lastLog = 0;

uint32_t Fnv(const void* p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) h = (h ^ ((const uint8_t*)p)[i]) * 16777619u;
    return h;
}


// ---- constant buffers a shader really declares (DXBC SHDR/SHEX dcl_constantbuffer). Only those slots are searched
// for a camera and patched, and never a dynamically indexed one (instance / bone arrays). GreedFall VS CD10CF14 reads
// per-instance matrices from cb4[4096] (dynamic index); a camera layout learned for slot 4 from another shader's
// leftover binding made the mod write a projection into rows 47-50 of every batch's instance array -> instances
// thrown to infinity (phantom sails / poles / slabs = the user's "haze", tests 069-077).
struct CbDecl { uint16_t declared = 0, dynamic = 0; bool parsed = false; };
std::unordered_map<uint32_t, CbDecl> g_vsDecl, g_psDecl;   // by shader hash (under g_mx)
static CbDecl ParseCbDecl(const void* code, size_t n) {
    CbDecl d;
    const uint8_t* b = (const uint8_t*)code;
    if (n < 32 || memcmp(b, "DXBC", 4)) return d;
    uint32_t chunks = *(const uint32_t*)(b + 28);
    for (uint32_t i = 0; i < chunks && 32 + i * 4 + 4 <= n; ++i) {
        uint32_t off = *(const uint32_t*)(b + 32 + i * 4);
        if (off + 8 > n) continue;
        if (memcmp(b + off, "SHDR", 4) && memcmp(b + off, "SHEX", 4)) continue;
        uint32_t sz = *(const uint32_t*)(b + off + 4);
        const uint32_t* t = (const uint32_t*)(b + off + 8);
        uint32_t nt = sz / 4;
        if (nt < 2) return d;
        uint32_t len = t[1] < nt ? t[1] : nt;
        for (uint32_t pos = 2; pos < len;) {
            uint32_t tok = t[pos], op = tok & 0x7FF, il = (tok >> 24) & 0x7F;
            if (op == 0x23) il = pos + 1 < len ? t[pos + 1] : 0;   // customdata: length in the next token
            if (il == 0) break;
            if (op == 0x59 && pos + 3 < len) {   // dcl_constantbuffer: [opcode][operand][register][size]
                uint32_t reg = t[pos + 2];
                if (reg < 16) { d.declared |= (uint16_t)(1u << reg); if ((tok >> 11) & 1) d.dynamic |= (uint16_t)(1u << reg); }
            }
            pos += il;
        }
        d.parsed = true;
        return d;
    }
    return d;
}

// ---- device hooks
HRESULT STDMETHODCALLTYPE Hk_CreateBuffer(ID3D11Device* d, const D3D11_BUFFER_DESC* desc, const D3D11_SUBRESOURCE_DATA* init, ID3D11Buffer** out) {
    if (g_bypass11) return o_D11CreateBuffer(d, desc, init, out);
    HRESULT hr = o_D11CreateBuffer(d, desc, init, out);
    if (SUCCEEDED(hr) && out && *out && desc && (desc->BindFlags & D3D11_BIND_CONSTANT_BUFFER)) {
        MxGuard l;
        InterlockedIncrement(&g_cbGen);
        Shadow& s = g_cb[*out];   // a new buffer may reuse a released one's address
        s.bytes = desc->ByteWidth;
        s.f.assign((desc->ByteWidth + 15) / 16 * 4, 0.f);
        s.mapped = nullptr;
        if (init && init->pSysMem) memcpy(s.f.data(), init->pSysMem, desc->ByteWidth);
        s.ver = NextVer();
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE Hk_CreateVertexShader(ID3D11Device* d, const void* code, SIZE_T n, ID3D11ClassLinkage* l, ID3D11VertexShader** out) {
    if (g_bypass11) return o_D11CreateVertexShader(d, code, n, l, out);
    HRESULT hr = o_D11CreateVertexShader(d, code, n, l, out);
    if (SUCCEEDED(hr) && out && *out) {
        uint32_t h = Fnv(code, n);
        CbDecl d = ParseCbDecl(code, n);
        { MxGuard g; g_vsHash[*out] = h; g_vsDecl[h] = d; }
        { static int nl = 0; char hs2[12]; snprintf(hs2, sizeof hs2, "%08X", h);
          if (nl < 3 || (!G().s.stereoIsoVs.empty() && G().s.stereoIsoVs == hs2)) { nl++; LOGI("d3d11: VS %08X declares constant buffers %04x (dynamically indexed %04x)%s", h, d.declared, d.dynamic, d.parsed ? "" : " - not parsed"); } }
        // test aid: dump_vs = "HASH,HASH" -> GFVR\run_*\vs_HASH.dxbc (bytecode, for offline disassembly)
        char hs[16]; snprintf(hs, sizeof hs, "%08X", h);
        if (!G().s.dumpVs.empty() && (G().s.dumpVs == "*" || G().s.dumpVs.find(hs) != std::string::npos)) {
            wchar_t path[MAX_PATH];
            swprintf_s(path, L"%svs_%S.dxbc", G().runDir.c_str(), hs);
            if (FILE* f = _wfopen(path, L"wb")) { fwrite(code, 1, n, f); fclose(f); LOGI("d3d11: dumped VS %s (%zu bytes)", hs, (size_t)n); }
        }
    }
    return hr;
}
// GreedFall depth of field: PS EEDD5866 (pp_dof composite, disassembled test 080): if cb1[10].y > 0 it blends the sharp
// image toward the blurred one (t8) by circle of confusion * cb1[10].y. game_dof off: that draw gets a copy of its cb1 with
// [10].y = 0 (no blur; the in-focus sharpening and colour grading of the same pass stay). RENDER\EnableDOF does not
// reach cutscene DoF (test 109: no change), the cutscene post-process sets these values itself.
static ID3D11PixelShader* volatile g_dofPs = nullptr;
static volatile LONG g_dofPatched = 0;
HRESULT STDMETHODCALLTYPE Hk_CreatePixelShader(ID3D11Device* d, const void* code, SIZE_T n, ID3D11ClassLinkage* l, ID3D11PixelShader** out) {
    if (g_bypass11) return o_D11CreatePixelShader(d, code, n, l, out);
    HRESULT hr = o_D11CreatePixelShader(d, code, n, l, out);
    if (SUCCEEDED(hr) && out && *out) {
        uint32_t h = Fnv(code, n); CbDecl dc = ParseCbDecl(code, n);
        { MxGuard g; g_psHash[*out] = h; g_psDecl[h] = dc; }
        if (h == 0xEEDD5866u) { g_dofPs = *out; LOGI("d3d11: depth-of-field composite shader found (PS EEDD5866)"); }
        char hs[16]; snprintf(hs, sizeof hs, "%08X", h);   // test aid: dump_vs also dumps pixel shaders of the list
        if (!G().s.dumpVs.empty() && G().s.dumpVs != "*" && G().s.dumpVs.find(hs) != std::string::npos) {
            wchar_t path[MAX_PATH];
            swprintf_s(path, L"%sps_%S.dxbc", G().runDir.c_str(), hs);
            if (FILE* f = _wfopen(path, L"wb")) { fwrite(code, 1, n, f); fclose(f); LOGI("d3d11: dumped PS %s (%zu bytes)", hs, (size_t)n); }
        }
    }
    return hr;
}

// ---- immediate-context hooks
struct Bind11;
void NoteTargets(Bind11& b, UINT n, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv);
// Hull / domain / geometry stages: GreedFall's terrain is tessellated - its vertex shader only outputs a 2D grid point and a
// tessellation factor, the camera projection (and patch culling) happen in the hull / domain shaders. Their constant
// buffers get the same per-eye camera copies as the vertex shader wherever the game bound the same buffer (the engine
// binds its global camera buffer to every stage). Without this the terrain was drawn with the game's own camera: head-locked.
void STDMETHODCALLTYPE Hk_HSSetConstantBuffers(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11Buffer* const* b) {
    if (!g_bypass11) if (Ctx11* x = X(c)) for (UINT i = 0; i < n && s + i < 14; ++i) x->tessCB[0][s + i] = b ? b[i] : nullptr;
    OC(c, HSSetConstantBuffers)(c, s, n, b);
}
void STDMETHODCALLTYPE Hk_DSSetConstantBuffers(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11Buffer* const* b) {
    if (!g_bypass11) if (Ctx11* x = X(c)) for (UINT i = 0; i < n && s + i < 14; ++i) x->tessCB[1][s + i] = b ? b[i] : nullptr;
    OC(c, DSSetConstantBuffers)(c, s, n, b);
}
void STDMETHODCALLTYPE Hk_GSSetConstantBuffers(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11Buffer* const* b) {
    if (!g_bypass11) if (Ctx11* x = X(c)) for (UINT i = 0; i < n && s + i < 14; ++i) x->tessCB[2][s + i] = b ? b[i] : nullptr;
    OC(c, GSSetConstantBuffers)(c, s, n, b);
}
static void BindOtherStages(ID3D11DeviceContext* c, Ctx11* x, ID3D11Buffer* game, ID3D11Buffer* use) {
    if (!game) return;
    for (int st = 0; st < 3; ++st)
        for (UINT k = 0; k < 14; ++k)
            if (x->tessCB[st][k] == game) {
                if (st == 0) OC(c, HSSetConstantBuffers)(c, k, 1, &use);
                else if (st == 1) OC(c, DSSetConstantBuffers)(c, k, 1, &use);
                else OC(c, GSSetConstantBuffers)(c, k, 1, &use);
            }
}
void STDMETHODCALLTYPE Hk_VSSetConstantBuffers(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11Buffer* const* b) {
    if (g_bypass11) return OC(c, VSSetConstantBuffers)(c, s, n, b);
    if (Ctx11* x = X(c)) for (UINT i = 0; i < n && s + i < 14; ++i) x->vsCB[s + i] = b ? b[i] : nullptr;
    OC(c, VSSetConstantBuffers)(c, s, n, b);
}
void STDMETHODCALLTYPE Hk_VSSetShader(ID3D11DeviceContext* c, ID3D11VertexShader* vs, ID3D11ClassInstance* const* ci, UINT n) {
    if (g_bypass11) return OC(c, VSSetShader)(c, vs, ci, n);
    if (Ctx11* x = X(c)) x->curVS = vs;
    OC(c, VSSetShader)(c, vs, ci, n);
}
void STDMETHODCALLTYPE Hk_UpdateSubresource(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* data, UINT rp, UINT dp) {
    if (g_bypass11) return OC(c, UpdateSubresource)(c, r, sub, box, data, rp, dp);
    HookTimer ht_;
    if (data && sub == 0) {
        Ctx11* x = X(c);
        MxGuard l;
        if (Shadow* sh = WriteShadow(x, (ID3D11Buffer*)r)) {
            UINT off = box ? box->left : 0, end = box ? box->right : sh->bytes;
            if (end > sh->bytes) end = sh->bytes;
            if (G().s.skipBigCb && sh->bytes > (UINT)G().s.camCbMaxBytes) InterlockedAdd64(&g_pfSkipped, end > off ? end - off : 0);
            else if (end > off) { memcpy((uint8_t*)sh->f.data() + off, data, end - off); InterlockedAdd64(&g_pfCopied, end - off); }
            sh->ver = NextVer();
            g_st.cbUpdates++;
        }
    }
    OC(c, UpdateSubresource)(c, r, sub, box, data, rp, dp);
}
HRESULT STDMETHODCALLTYPE Hk_Map(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, D3D11_MAP t, UINT fl, D3D11_MAPPED_SUBRESOURCE* m) {
    if (g_bypass11) return OC(c, Map)(c, r, sub, t, fl, m);
    HookTimer ht_;
    HRESULT hr = OC(c, Map)(c, r, sub, t, fl, m);
    if (SUCCEEDED(hr) && m && sub == 0 && (t == D3D11_MAP_WRITE_DISCARD || t == D3D11_MAP_WRITE_NO_OVERWRITE || t == D3D11_MAP_WRITE)) {
        Ctx11* x = X(c);
        Shadow* fs = nullptr; int fr = WriteShadowFast(x, (ID3D11Buffer*)r, &fs);
        if (fr == kFastCb) fs->mapped = m->pData;
        else if (fr == kFastMiss) {
            MxGuard l;
            if (Shadow* sh = WriteShadow(x, (ID3D11Buffer*)r)) sh->mapped = m->pData;
        }
    }
    return hr;
}
void STDMETHODCALLTYPE Hk_Unmap(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub) {
    if (g_bypass11) return OC(c, Unmap)(c, r, sub);
    HookTimer ht_;
    if (sub == 0) {
        Ctx11* x = X(c);
        Shadow* sh = nullptr; int fr = WriteShadowFast(x, (ID3D11Buffer*)r, &sh);
        std::unique_lock<SpinMx> l(g_mx, std::defer_lock);
        if (fr == kFastMiss) { l.lock(); sh = WriteShadow(x, (ID3D11Buffer*)r); }
        if (sh && sh->mapped && G().s.skipBigCb && sh->bytes > (UINT)G().s.camCbMaxBytes) { sh->mapped = nullptr; InterlockedAdd64(&g_pfSkipped, sh->bytes); }
        else if (sh && sh->mapped) {
            InterlockedAdd64(&g_pfCopied, sh->bytes);
            memcpy(sh->f.data(), sh->mapped, sh->bytes);   // the game wrote all of it (DISCARD)
            sh->mapped = nullptr;
            sh->ver = NextVer();
            g_st.cbMaps++;
        }
    }
    OC(c, Unmap)(c, r, sub);
}
void STDMETHODCALLTYPE Hk_OMSetRenderTargets(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv) {
    if (g_bypass11) return OC(c, OMSetRenderTargets)(c, n, rtv, dsv);
    if (Ctx11* x = X(c)) { g_st.rtSets++; NoteTargets(x->b, n, rtv, dsv); }
    OC(c, OMSetRenderTargets)(c, n, rtv, dsv);
}

// ======================================================================== stage 3: stereo
// Same scheme as the D3D9 native method (stereo.cpp), with D3D11 objects:
//  - every non-square render target / depth texture gets a right-eye twin (attached as private data, so it lives as
//    long as the game's texture); views created on a twinned texture get twin views;
//  - a draw into twinned targets is issued twice: left eye into the game's targets, right eye into the twins with
//    twinned shader resources; draws with a camera found by value get per-eye copies of their constant buffers
//    (ViewProjection, CameraPosition, ScreenToWorld, inverse VP replaced);
//  - clears / copies / resolves of twinned resources are mirrored;
//  - the first draw (or copy) into the back buffer that reads a twinned texture after a 3D scene is the composite: it
//    is drawn again into two eye textures (left: game's textures, right: twins). Later back-buffer draws (the UI) are
//    drawn into both eye textures as well (head-locked at screen depth).
const GUID kTwin11 = {0x2f6d1a90, 0x7c3e, 0x4b51, {0x8e, 0x14, 0x5a, 0xd2, 0x3b, 0x97, 0x60, 0xc1}};
int g_privAddRef11 = -1;
unsigned g_twins11 = 0;
bool g_twinFailed11 = false;

template <class T> T* Twin11(T* r) {
    if (!r) return nullptr;
    IUnknown* p = nullptr;
    UINT sz = sizeof(p);
    if (FAILED(r->GetPrivateData(kTwin11, &sz, &p)) || !p || sz != sizeof(p)) return nullptr;
    if (g_privAddRef11 == 1) p->Release();   // the game's object keeps it alive
    return (T*)p;
}
void Attach11(ID3D11DeviceChild* orig, IUnknown* twin) {
    orig->SetPrivateDataInterface(kTwin11, twin);
    if (g_privAddRef11 < 0) {
        twin->AddRef();
        ULONG before = twin->Release();
        IUnknown* p = nullptr;
        UINT sz = sizeof(p);
        orig->GetPrivateData(kTwin11, &sz, &p);
        twin->AddRef();
        ULONG after = twin->Release();
        g_privAddRef11 = after > before ? 1 : 0;
        if (g_privAddRef11 == 1) twin->Release();
        LOGI("d3d11 stereo: GetPrivateData %s", g_privAddRef11 ? "AddRefs" : "does not AddRef");
    }
}
bool StereoOn11() { return G().s.vrEnabled && G().s.d3d11Stereo && G().s.d3d11Vr; }
bool WantTwin11(const D3D11_TEXTURE2D_DESC* d) {
    if (!d || !StereoOn11()) return false;
    if (!(d->BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_UNORDERED_ACCESS))) return false;
    if (d->ArraySize != 1 || (d->MiscFlags & (D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_TEXTURECUBE)))
        return false;
    return d->Width != d->Height && d->Width >= 64 && d->Height >= 32;   // square: shadow maps, reflections (eye-independent)
}

HRESULT STDMETHODCALLTYPE Hk_CreateTexture2D(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC* d, const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D** out) {
    if (g_bypass11) return o_D11CreateTexture2D(dev, d, init, out);
    HRESULT hr = o_D11CreateTexture2D(dev, d, init, out);
    if (SUCCEEDED(hr) && out && *out && WantTwin11(d)) {
        ID3D11Texture2D* t = nullptr;
        if (SUCCEEDED(o_D11CreateTexture2D(dev, d, nullptr, &t)) && t) {
            Attach11(*out, t);
            t->Release();
            if (g_twins11++ < 12)
                LOGI("d3d11 stereo: twin %ux%u format %d bind %x samples %u", d->Width, d->Height, (int)d->Format, d->BindFlags, d->SampleDesc.Count);
        } else if (!g_twinFailed11) {
            g_twinFailed11 = true;
            LOGE("d3d11 stereo: twin %ux%u format %d failed", d->Width, d->Height, (int)d->Format);
        }
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE Hk_CreateRenderTargetView(ID3D11Device* dev, ID3D11Resource* r, const D3D11_RENDER_TARGET_VIEW_DESC* d, ID3D11RenderTargetView** out) {
    if (g_bypass11) return o_D11CreateRenderTargetView(dev, r, d, out);
    HRESULT hr = o_D11CreateRenderTargetView(dev, r, d, out);
    if (SUCCEEDED(hr) && out && *out)
        if (ID3D11Resource* t = Twin11(r)) {
            ID3D11RenderTargetView* v = nullptr;
            if (SUCCEEDED(o_D11CreateRenderTargetView(dev, t, d, &v)) && v) { Attach11(*out, v); v->Release(); }
        }
    return hr;
}
HRESULT STDMETHODCALLTYPE Hk_CreateShaderResourceView(ID3D11Device* dev, ID3D11Resource* r, const D3D11_SHADER_RESOURCE_VIEW_DESC* d, ID3D11ShaderResourceView** out) {
    if (g_bypass11) return o_D11CreateShaderResourceView(dev, r, d, out);
    HRESULT hr = o_D11CreateShaderResourceView(dev, r, d, out);
    if (SUCCEEDED(hr) && out && *out)
        if (ID3D11Resource* t = Twin11(r)) {
            ID3D11ShaderResourceView* v = nullptr;
            if (SUCCEEDED(o_D11CreateShaderResourceView(dev, t, d, &v)) && v) { Attach11(*out, v); v->Release(); }
        }
    return hr;
}
HRESULT STDMETHODCALLTYPE Hk_CreateDepthStencilView(ID3D11Device* dev, ID3D11Resource* r, const D3D11_DEPTH_STENCIL_VIEW_DESC* d, ID3D11DepthStencilView** out) {
    if (g_bypass11) return o_D11CreateDepthStencilView(dev, r, d, out);
    HRESULT hr = o_D11CreateDepthStencilView(dev, r, d, out);
    if (SUCCEEDED(hr) && out && *out)
        if (ID3D11Resource* t = Twin11(r)) {
            ID3D11DepthStencilView* v = nullptr;
            if (SUCCEEDED(o_D11CreateDepthStencilView(dev, t, d, &v)) && v) { Attach11(*out, v); v->Release(); }
        }
    return hr;
}

// ---- what the game has bound (immediate context), with the twins looked up once at bind time
ID3D11Resource* g_bbRes = nullptr;   // the swap chain's buffer 0 (no reference; refreshed at Present)
bool g_warnedSharedDepth = false;
unsigned g_uavBinds = 0;   // OMSetRenderTargetsAndUnorderedAccessViews with UAVs (diagnostics)

void NoteTargets(Bind11& B, UINT n, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv) {
    B.nrtv = n > 8 ? 8 : n;
    for (UINT i = 0; i < 8; ++i) {
        B.rtv[i] = i < B.nrtv && rtv ? rtv[i] : nullptr;
        B.rtvT[i] = Twin11(B.rtv[i]);
    }
    B.dsv = dsv;
    B.dsvT = Twin11(dsv);
    B.rt0IsBB = false;
    if (B.rtv[0] && g_bbRes) {
        ID3D11Resource* r = nullptr;
        B.rtv[0]->GetResource(&r);
        B.rt0IsBB = r == g_bbRes;
        if (r) r->Release();
    }
}
void STDMETHODCALLTYPE Hk_OMSetRenderTargetsAndUnorderedAccessViews(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtv,
                                                                     ID3D11DepthStencilView* dsv, UINT us, UINT un,
                                                                     ID3D11UnorderedAccessView* const* uav, const UINT* cnt) {
    if (g_bypass11) return OC(c, OMSetRenderTargetsAndUnorderedAccessViews)(c, n, rtv, dsv, us, un, uav, cnt);
    Ctx11* x = X(c);
    if (x && n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) { g_st.rtSets++; NoteTargets(x->b, n, rtv, dsv); }
    if (x && un && un != D3D11_KEEP_UNORDERED_ACCESS_VIEWS && uav) g_uavBinds++;
    OC(c, OMSetRenderTargetsAndUnorderedAccessViews)(c, n, rtv, dsv, us, un, uav, cnt);
}
void STDMETHODCALLTYPE Hk_PSSetShaderResources(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11ShaderResourceView* const* v) {
    if (g_bypass11) return OC(c, PSSetShaderResources)(c, s, n, v);
    if (Ctx11* x = X(c))
        for (UINT i = 0; i < n && s + i < kSrv; ++i) {
            UINT k = s + i;
            Bind11& B = x->b;
            if (B.srvT[k]) B.nTwinSrv--;
            B.srv[k] = v ? v[i] : nullptr;
            B.srvT[k] = Twin11(B.srv[k]);
            if (B.srvT[k]) B.nTwinSrv++;
            if (B.srv[k] && k + 1 > B.maxSrv) B.maxSrv = k + 1;
        }
    OC(c, PSSetShaderResources)(c, s, n, v);
}
void STDMETHODCALLTYPE Hk_PSSetConstantBuffers(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11Buffer* const* b) {
    if (g_bypass11) return OC(c, PSSetConstantBuffers)(c, s, n, b);
    if (Ctx11* x = X(c)) for (UINT i = 0; i < n && s + i < 14; ++i) x->psCB[s + i] = b ? b[i] : nullptr;
    OC(c, PSSetConstantBuffers)(c, s, n, b);
}
void STDMETHODCALLTYPE Hk_PSSetShader(ID3D11DeviceContext* c, ID3D11PixelShader* ps, ID3D11ClassInstance* const* ci, UINT n) {
    if (g_bypass11) return OC(c, PSSetShader)(c, ps, ci, n);
    if (Ctx11* x = X(c)) x->curPS = ps;
    OC(c, PSSetShader)(c, ps, ci, n);
}

// ---- per-frame state
bool D3d11MainCameraPos(float out[3]);
struct Frame11 {
    bool active = false;      // poses known, host connected, stereo on
    bool patch = false;       // eye cameras applied this frame (the previous frame was shown in stereo)
    bool composited = false;  // eye images rendered this frame
    unsigned camDraws = 0, patched = 0, right = 0, mirrored = 0, hud = 0;
    unsigned dispatchMirrored = 0, dispatch = 0, indirect = 0, uavBinds = 0, vsTwinSrv = 0, sharedOut = 0;   // diagnostics: not mirrored (yet)
    bool haveMain = false;    // the frame's first world camera
    ue3::M4 mainVP, mainS2W, mainIVP;
    ue3::Camera mainCam;
    EyeMats11 mainEm;
    bool mainEmOk = false;
    bool uiTex = false;       // the HUD texture was filled from the game's UI texture this frame
} g_f;
bool g_lastStereo11 = false, g_want11 = false;
unsigned g_lastPatched11 = 0;
struct Stats11 { uint64_t frames = 0, stereo = 0, patched = 0, right = 0, mirrored = 0, hud = 0, dispatch = 0, dispatchM = 0, indirect = 0, uavBinds = 0, vsTwinSrv = 0, sharedOut = 0; } g_s11;

// eye textures (composite target): back-buffer sized, typeless so views can match the game's back-buffer view format
ID3D11Texture2D* g_eyeTex[2] = {};
ID3D11Texture2D* g_hudTex = nullptr;   // UI drawn after the scene composite (transparent, premultiplied): the HUD layer
struct HudRtv { DXGI_FORMAT f; ID3D11RenderTargetView* v; };
std::vector<HudRtv> g_hudRtv;
std::unordered_map<ID3D11BlendState*, ID3D11BlendState*> g_hudBlend;   // game blend state -> same with alpha accumulation
UINT g_eyeW = 0, g_eyeH = 0;
DXGI_FORMAT g_eyeFmt = DXGI_FORMAT_UNKNOWN;
struct EyeRtv { DXGI_FORMAT f; ID3D11RenderTargetView* v[2]; };
std::vector<EyeRtv> g_eyeRtv;
void ReleaseEyes() {
    for (auto& e : g_eyeRtv) for (int i = 0; i < 2; ++i) if (e.v[i]) e.v[i]->Release();
    g_eyeRtv.clear();
    for (int i = 0; i < 2; ++i) { if (g_eyeTex[i]) g_eyeTex[i]->Release(); g_eyeTex[i] = nullptr; }
    for (auto& h : g_hudRtv) if (h.v) h.v->Release();
    g_hudRtv.clear();
    if (g_hudTex) { g_hudTex->Release(); g_hudTex = nullptr; }
    for (auto& kv : g_hudBlend) { if (kv.second) kv.second->Release(); kv.first->Release(); }
    g_hudBlend.clear();
    g_eyeW = g_eyeH = 0;
}
DXGI_FORMAT Typeless(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_TYPELESS;
    default: return f;
    }
}
// ---- LumaSharpen on the eye images (sharedtools\shaders\lumasharpen, CeeJay.dk / ReShade, MIT), once per presented
// frame right before they go to the headset. Works on the stored (gamma-encoded) values through UNORM views, like
// ReShade on a back buffer.
bool EnsureFsShaders();
extern ID3D11VertexShader* g_fsVS;
static ID3D11PixelShader* g_shPS = nullptr; static ID3D11Buffer* g_shCB = nullptr; static bool g_shFailed = false;
static ID3D11Texture2D* g_shTmp = nullptr; static ID3D11RenderTargetView* g_shTmpRtv = nullptr;
static ID3D11ShaderResourceView* g_shSrv[2] = {}; static ID3D11Texture2D* g_shFor = nullptr;
static DXGI_FORMAT UnormOf(DXGI_FORMAT tf) {
    switch (tf) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    default: return tf;
    }
}
void SharpenEyes11(ID3D11DeviceContext* c) {
    float k = G().s.sharpen;
    if (k <= 0.001f || g_shFailed || !g_eyeTex[0] || !g_eyeTex[1] || !EnsureFsShaders()) return;
    Bypass11 bp;
    if (!g_shPS) {
        typedef HRESULT(WINAPI* Compile_t)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
        HMODULE m = LoadLibraryA("d3dcompiler_47.dll");
        Compile_t comp = m ? (Compile_t)GetProcAddress(m, "D3DCompile") : nullptr;
        const char* src =
            "Texture2D<float4> src : register(t0);"
            "cbuffer P : register(b0) { float sharpStrength; float sharpClamp; float2 padding; };"
            "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {"
            "  int2 p = int2(pos.xy); uint w, h; src.GetDimensions(w, h); const int2 hi = int2(w, h) - 1;"
            "  const float4 ori = src.Load(int3(p, 0));"
            "  float3 blur = ori.rgb * 0.25;"
            "  blur += (src.Load(int3(clamp(p + int2(-1, 0), 0, hi), 0)).rgb + src.Load(int3(clamp(p + int2(1, 0), 0, hi), 0)).rgb +"
            "           src.Load(int3(clamp(p + int2(0, -1), 0, hi), 0)).rgb + src.Load(int3(clamp(p + int2(0, 1), 0, hi), 0)).rgb) * 0.125;"
            "  blur += (src.Load(int3(clamp(p + int2(-1, -1), 0, hi), 0)).rgb + src.Load(int3(clamp(p + int2(1, -1), 0, hi), 0)).rgb +"
            "           src.Load(int3(clamp(p + int2(-1, 1), 0, hi), 0)).rgb + src.Load(int3(clamp(p + int2(1, 1), 0, hi), 0)).rgb) * 0.0625;"
            "  const float3 sharp = ori.rgb - blur;"
            "  const float3 lumaStrength = float3(0.2126, 0.7152, 0.0722) * sharpStrength;"
            "  const float4 lumaClamp = float4(lumaStrength * (0.5 / sharpClamp), 0.5);"
            "  float sharpLuma = saturate(dot(float4(sharp, 1.0), lumaClamp));"
            "  sharpLuma = (sharpClamp * 2.0) * sharpLuma - sharpClamp;"
            "  return float4(ori.rgb + sharpLuma, ori.a); }";
        ID3DBlob *b = nullptr, *err = nullptr;
        if (!comp || FAILED(comp(src, strlen(src), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &b, &err)) ||
            FAILED(g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_shPS))) {
            LOGW("d3d11: sharpening shader failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
            if (err) err->Release(); if (b) b->Release(); g_shFailed = true; return;
        }
        b->Release();
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(g_dev->CreateBuffer(&bd, nullptr, &g_shCB))) { g_shFailed = true; return; }
        LOGI("d3d11: LumaSharpen ready");
    }
    if (g_shFor != g_eyeTex[0]) {   // (re)create views and the scratch target for the current eye textures
        for (auto*& v : g_shSrv) { if (v) v->Release(); v = nullptr; }
        if (g_shTmpRtv) g_shTmpRtv->Release(); if (g_shTmp) g_shTmp->Release(); g_shTmpRtv = nullptr; g_shTmp = nullptr; g_shFor = nullptr;
        D3D11_TEXTURE2D_DESC d{}; g_eyeTex[0]->GetDesc(&d);
        DXGI_FORMAT f = UnormOf(d.Format);
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{}; sv.Format = f; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
        D3D11_RENDER_TARGET_VIEW_DESC rv{}; rv.Format = f; rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(g_dev->CreateShaderResourceView(g_eyeTex[0], &sv, &g_shSrv[0])) || FAILED(g_dev->CreateShaderResourceView(g_eyeTex[1], &sv, &g_shSrv[1])) ||
            FAILED(o_D11CreateTexture2D(g_dev, &d, nullptr, &g_shTmp)) || FAILED(g_dev->CreateRenderTargetView(g_shTmp, &rv, &g_shTmpRtv))) {
            LOGW("d3d11: sharpening views failed (format %d)", (int)d.Format); g_shFailed = true; return;
        }
        g_shFor = g_eyeTex[0];
        LOGI("d3d11: LumaSharpen on %ux%u eye images (view format %d)", d.Width, d.Height, (int)f);
    }
    float cb[4] = {k, G().s.sharpenClamp > 0.001f ? G().s.sharpenClamp : 0.035f, 0, 0};
    // save the state we touch (the game's own state at Present)
    ID3D11VertexShader* vs = nullptr; ID3D11PixelShader* ps = nullptr; ID3D11InputLayout* il = nullptr; D3D11_PRIMITIVE_TOPOLOGY topo;
    ID3D11BlendState* bs = nullptr; FLOAT bf[4]; UINT mask = 0; ID3D11DepthStencilState* ds = nullptr; UINT sref = 0; ID3D11RasterizerState* rs = nullptr;
    UINT nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE; D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    ID3D11ShaderResourceView* srv0 = nullptr; ID3D11Buffer* cb0 = nullptr; ID3D11RenderTargetView* rtv[8] = {}; ID3D11DepthStencilView* dsv = nullptr;
    c->OMGetRenderTargets(8, rtv, &dsv);
    c->VSGetShader(&vs, nullptr, nullptr); c->PSGetShader(&ps, nullptr, nullptr); c->IAGetInputLayout(&il); c->IAGetPrimitiveTopology(&topo);
    c->OMGetBlendState(&bs, bf, &mask); c->OMGetDepthStencilState(&ds, &sref); c->RSGetState(&rs); c->RSGetViewports(&nvp, vps);
    c->PSGetShaderResources(0, 1, &srv0); c->PSGetConstantBuffers(0, 1, &cb0);
    c->UpdateSubresource(g_shCB, 0, nullptr, cb, 0, 0);
    D3D11_VIEWPORT v{0, 0, (float)g_eyeW, (float)g_eyeH, 0, 1};
    c->VSSetShader(g_fsVS, nullptr, 0); c->PSSetShader(g_shPS, nullptr, 0); c->IASetInputLayout(nullptr);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); c->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    c->OMSetDepthStencilState(nullptr, 0); c->RSSetState(nullptr); c->RSSetViewports(1, &v); c->PSSetConstantBuffers(0, 1, &g_shCB);
    for (int e = 0; e < 2; ++e) {
        ID3D11ShaderResourceView* nul = nullptr;
        c->OMSetRenderTargets(1, &g_shTmpRtv, nullptr);
        c->PSSetShaderResources(0, 1, &g_shSrv[e]);
        c->Draw(3, 0);
        c->PSSetShaderResources(0, 1, &nul);
        c->OMSetRenderTargets(0, nullptr, nullptr);
        c->CopyResource(g_eyeTex[e], g_shTmp);
    }
    c->OMSetRenderTargets(8, rtv, dsv);
    c->VSSetShader(vs, nullptr, 0); c->PSSetShader(ps, nullptr, 0); c->IASetInputLayout(il); c->IASetPrimitiveTopology(topo);
    c->OMSetBlendState(bs, bf, mask); c->OMSetDepthStencilState(ds, sref); c->RSSetState(rs); c->RSSetViewports(nvp, vps);
    c->PSSetShaderResources(0, 1, &srv0); c->PSSetConstantBuffers(0, 1, &cb0);
    for (auto* r : rtv) if (r) r->Release(); if (dsv) dsv->Release();
    if (vs) vs->Release(); if (ps) ps->Release(); if (il) il->Release(); if (bs) bs->Release(); if (ds) ds->Release(); if (rs) rs->Release();
    if (srv0) srv0->Release(); if (cb0) cb0->Release();
}
bool EnsureEyes(UINT w, UINT h, DXGI_FORMAT bbFmt) {
    DXGI_FORMAT tf = Typeless(bbFmt);
    if (g_eyeTex[0] && g_eyeW == w && g_eyeH == h && g_eyeFmt == tf) return true;
    ReleaseEyes();
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = tf; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    for (int i = 0; i < 2; ++i)
        if (FAILED(o_D11CreateTexture2D(g_dev, &d, nullptr, &g_eyeTex[i]))) {
            LOGE("d3d11 stereo: eye texture %ux%u format %d failed", w, h, (int)tf);
            ReleaseEyes();
            return false;
        }
    if (FAILED(o_D11CreateTexture2D(g_dev, &d, nullptr, &g_hudTex))) { LOGE("d3d11 stereo: HUD texture failed"); g_hudTex = nullptr; }
    g_eyeW = w; g_eyeH = h; g_eyeFmt = tf;
    LOGI("d3d11 stereo: eye textures %ux%u format %d (+ HUD texture)", w, h, (int)tf);
    return true;
}
ID3D11RenderTargetView* EyeView(int e, ID3D11RenderTargetView* game) {
    D3D11_RENDER_TARGET_VIEW_DESC gd{};
    game->GetDesc(&gd);
    DXGI_FORMAT f = gd.Format == DXGI_FORMAT_B8G8R8X8_UNORM ? DXGI_FORMAT_B8G8R8A8_UNORM : gd.Format;
    for (auto& x : g_eyeRtv) if (x.f == f) return x.v[e];
    EyeRtv x{f, {nullptr, nullptr}};
    D3D11_RENDER_TARGET_VIEW_DESC vd{};
    vd.Format = f; vd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    for (int i = 0; i < 2; ++i)
        if (FAILED(o_D11CreateRenderTargetView(g_dev, g_eyeTex[i], &vd, &x.v[i]))) {
            LOGE("d3d11 stereo: eye view format %d failed", (int)f);
            x.v[i] = nullptr;
        }
    g_eyeRtv.push_back(x);
    return x.v[e];
}

// ---- per-eye constant buffers

struct SlotPatch { bool ps; int slot; ue3::CbLayout L; ID3D11Buffer* buf; bool ownVP; };
struct DrawCam { int n = 0; SlotPatch s[8]; bool haveVP = false; ue3::M4 VP; };

bool RowsNear(const float* f4, int r, const ue3::M4& m, int rows, bool transposed) {
    for (int i = 0; i < rows; ++i)
        for (int j = 0; j < 4; ++j) {
            float want = transposed ? m.m[j][i] : m.m[i][j], got = f4[(r + i) * 4 + j];
            if (std::fabs(got - want) > 1e-6f + 2e-3f * std::fabs(want)) return false;
        }
    return true;
}
// A slot whose layout has no ViewProjection of its own: are the derived values still the main camera's?
bool VerifyDerived(const float* f4, int n, const ue3::CbLayout& L) {
    if (!g_f.haveMain) return false;
    bool any = false;
    if (L.s2w >= 0) { if (L.s2w + L.s2wN > n || !RowsNear(f4, L.s2w, g_f.mainS2W, L.s2wN, false)) return false; any = true; }
    if (L.ivp >= 0) { if (L.ivp + 4 > n || !RowsNear(f4, L.ivp, g_f.mainIVP, 4, L.ivpT)) return false; any = true; }
    if (L.vp >= 0) { if (L.vp + 4 > n || !RowsNear(f4, L.vp, g_f.mainVP, 4, L.vpT)) return false; any = true; }
    if (L.cam >= 0) {
        if (L.cam >= n) return false;
        for (int i = 0; i < 3; ++i) if (std::fabs(f4[L.cam * 4 + i] - g_f.mainCam.pos[i]) > 1e-2f + 1e-3f * std::fabs(g_f.mainCam.pos[i])) return false;
        any = true;
    }
    return any;
}
static float g_mainFovMin = 1e9f, g_mainFovMax = 0;   // vertical FOV of the main camera this status period
void SetMain(const ue3::M4& VP, const ue3::Camera& cam) {
    g_f.haveMain = true;
    if (cam.fovV > 1) { g_mainFovMin = std::min(g_mainFovMin, cam.fovV); g_mainFovMax = std::max(g_mainFovMax, cam.fovV); }
    g_f.mainVP = VP; g_f.mainCam = cam;
    ue3::M4 iVP;
    if (!ue3::Inverse(VP, &iVP)) { g_f.haveMain = false; return; }
    g_f.mainIVP = iVP;
    g_f.mainS2W = ue3::Mul(ue3::Q(cam), iVP);
    g_f.mainEmOk = g_f.active && StereoEyeMats11(&VP.m[0][0], &g_f.mainEm);
}

// Derived layouts are learned once per shader, from whatever the frame's main camera was at that draw; a field that did
// not match then (View / InvView / second previous VP) stayed missing for good. GreedFall's cb0 is one global struct
// (1616 bytes) bound to every pass, so layouts of the same buffer size and VP / camera rows are merged: the union of
// everything found by any shader (user build 16: the half-res AO PS 0C108836 had View -1 -> its depth tests used the
// game camera's view -> dark copies of nearby objects moving with head turns = the haze).
static std::unordered_map<uint64_t, ue3::CbLayout> g_bestLayout;   // (under g_mx) key: bytes, vp row, cam row
static bool FillLayout(ue3::CbLayout& d, const ue3::CbLayout& s) {
    bool ch = false;
#define FILL1(f) if (d.f < 0 && s.f >= 0) { d.f = s.f; ch = true; }
#define FILL2(f, a) if (d.f < 0 && s.f >= 0) { d.f = s.f; d.a = s.a; ch = true; }
#define FILL3(f, a, b) if (d.f < 0 && s.f >= 0) { d.f = s.f; d.a = s.a; d.b = s.b; ch = true; }
    FILL1(cam2) FILL2(ivp, ivpT) FILL3(v, vT, vS) FILL3(p, pT, pS) FILL3(iv, ivT2, ivS) FILL2(vp2, vp2T) FILL2(vp2b, vp2bT)
    FILL3(v2, v2T, v2S) FILL3(p2, p2T, p2S)
    if (d.s2w < 0 && s.s2w >= 0) { d.s2w = s.s2w; d.s2wN = s.s2wN; ch = true; }
#undef FILL1
#undef FILL2
#undef FILL3
    return ch;
}
static uint64_t LayoutKey(UINT bytes, const ue3::CbLayout& L) { return ((uint64_t)bytes << 32) | ((uint64_t)(uint16_t)L.vp << 16) | (uint16_t)L.cam; }
static void LearnLayout(UINT bytes, const ue3::CbLayout& L) {   // remember everything any shader found for this buffer
    if (L.vp < 0) return;
    ue3::CbLayout& B = g_bestLayout[LayoutKey(bytes, L)];
    if (B.vp < 0) B = L; else FillLayout(B, L);
}
static ue3::CbLayout Completed(UINT bytes, const ue3::CbLayout& L, uint32_t hash, bool ps) {   // L + what others found
    ue3::CbLayout C = L;
    if (!G().s.layoutMerge || L.vp < 0) return C;
    auto it = g_bestLayout.find(LayoutKey(bytes, L));
    if (it != g_bestLayout.end() && FillLayout(C, it->second)) {
        static std::unordered_set<uint32_t> said;
        if (said.insert(hash ^ (ps ? 0x80000000u : 0)).second && said.size() < 80)
            LOGI("d3d11: %s %08X: camera layout completed from other shaders: View %d, InvView %d, previous VP %d (+%d), View %d", ps ? "PS" : "VS", hash, C.v, C.iv, C.vp2, C.vp2b, C.v2);
    }
    return C;
}
// layout_repair: GreedFall's cb0 struct holds the current VP (row 8) and the previous frame's VP (63). A shader first
// seen on a draw whose row 8 is not a perspective camera (shadow pass) took row 63 as its VP, for good: only the
// previous-frame copy was patched -> that shader drawn with the game camera (user, build 25: the ground stuck to the
// HMD in some runs, fixed by a restart). A VP row that another layout of the same buffer size knows as the previous
// frame's VP is replaced by that layout (when its VP verifies in this buffer).
static const ue3::CbLayout* BetterLayout(UINT bytes, const ue3::CbLayout& L, const float* f4, int n) {
    if (!G().s.layoutRepair || L.vp < 0) return nullptr;
    for (auto& kv : g_bestLayout) {
        if ((UINT)(kv.first >> 32) != bytes) continue;
        const ue3::CbLayout& B = kv.second;
        if (B.vp < 0 || B.vp == L.vp || (B.vp2 != L.vp && B.vp2b != L.vp)) continue;
        if (ue3::VerifyViewProjection(f4, n, B, nullptr)) return &B;
    }
    return nullptr;
}
static uint32_t g_poisonVs = 0;   // test aid debug_layout_poison: this VS's first search skips rows 0-11 (reproduces the bad layout)
// Per draw: find (once per shader + slot) and then verify the camera in the bound constant buffers.
void ObserveSlots(Ctx11* x, bool ps, uint32_t hash, ShaderInfo& si, ID3D11Buffer* const* cbs, DrawCam* dc, Ctx11::DcKey* k = nullptr) {
    CbDecl decl;
    if (G().s.cbDeclFilter) { auto& m = ps ? g_psDecl : g_vsDecl; auto it = m.find(hash); if (it != m.end()) decl = it->second; }
    for (int s = 0; s < kCbSlots; ++s) {
        ID3D11Buffer* b = cbs[s];
        if (!b) continue;
        if (decl.parsed && (!(decl.declared & (1u << s)) || (decl.dynamic & (1u << s)))) continue;   // not read / indexed array
        Shadow* sh = FindShadow(x, b);
        if (!sh) continue;
        // Large buffers are per-instance data (GreedFall VS CD10CF14: 64 KB of instance matrices in cb1), never a
        // camera. A chance match there (an instance placed like the camera -> "InvView") made the mod replace that
        // draw's instance buffer with a patched copy of its shadow: objects drawn at other objects' places (phantom
        // poles / sails / slabs, test 074). Only buffers up to cam_cb_max_bytes are searched and patched.
        if (sh->bytes > (UINT)G().s.camCbMaxBytes) continue;
        const float* f4 = sh->f.data();
        int n = (int)(sh->f.size() / 4);
        ue3::CbLayout& L = si.slot[s];
        if (k) {
            if (L.vp >= 0 || L.any()) {   // the result depends on this buffer's contents: remembered for draw_cam_cache
                if (k->n < 16) { k->isPs[k->n] = ps; k->slot[k->n] = s; k->buf[k->n] = b; k->sh[k->n] = sh; k->ver[k->n] = sh->ver; k->n++; }
                else k->searched = true;
            } else if ((!ps && (si.tries <= 64 || (si.draws & 255) == 0)) || (g_f.haveMain && (si.dtries <= 64 || (si.draws & 255) == 0)))
                k->searched = true;   // a layout search runs now: its outcome is not reused
        }
        ue3::Camera cam;
        auto add = [&](bool ownVP) {
            if (dc && dc->n < 8) dc->s[dc->n++] = SlotPatch{ps, s, Completed(sh->bytes, L, hash, ps), b, ownVP};   // derived layouts with a VP row take the ownVP path too
        };
        if (L.vp >= 0 && !ps && (si.draws & 63) == 0) {
            if (const ue3::CbLayout* B = BetterLayout(sh->bytes, L, f4, n)) {
                LOGW("d3d11: VS %08X cb%d: camera at float4 %d was the previous frame's copy - now %d (layout repaired)", hash, s, L.vp, B->vp);
                L = *B;
            }
        }
        if (L.vp >= 0) {
            if (ue3::VerifyViewProjection(f4, n, L, &cam)) {
                si.camDraws++; g_st.camDraws++; si.fov = cam.fovH;
                ue3::M4 VP = ue3::LoadM4(f4, L.vp, L.vpT);
                if (!g_f.haveMain && (x->b.rtvT[0] || x->b.dsvT)) SetMain(VP, cam);   // eye-image draws only (not shadow / reflection views)
                if (dc && !dc->haveVP) { dc->haveVP = true; dc->VP = VP; }
                add(true);
            }
            continue;
        }
        if (L.any()) {   // derived values only (deferred passes: ScreenToWorld, CameraPosition)
            if (VerifyDerived(f4, n, L)) add(false);
            continue;
        }
        // first 64 draws, then every 256th: a shader first seen during loading (no valid camera yet) is not given up on
        if (!ps && (si.tries <= 64 || (si.draws & 255) == 0)) {
            si.tries++;
            bool found;
            if (g_poisonVs && hash == g_poisonVs && n > 12) {
                found = ue3::FindViewProjection(f4 + 48, n - 12, &L, &cam);
                if (found) { L.vp += 12; LOGW("d3d11: test aid: VS %08X layout poisoned (search from float4 12 -> %d)", hash, L.vp); g_poisonVs = 0; }
            } else found = ue3::FindViewProjection(f4, n, &L, &cam);
            if (found) {
                if (const ue3::CbLayout* B = BetterLayout(sh->bytes, L, f4, n)) {   // the previous frame's VP: use the current one
                    LOGI("d3d11: VS %08X cb%d: found the previous frame's VP at float4 %d - using %d", hash, s, L.vp, B->vp);
                    L = *B; ue3::VerifyViewProjection(f4, n, L, &cam);
                } else
                ue3::FindDerived(f4, n, ue3::LoadM4(f4, L.vp, L.vpT), cam, &L, L.vp);
                LearnLayout(sh->bytes, L);
                si.camDraws++; g_st.camDraws++; si.fov = cam.fovH;
                ue3::M4 VP = ue3::LoadM4(f4, L.vp, L.vpT);
                if (!g_f.haveMain && (x->b.rtvT[0] || x->b.dsvT)) SetMain(VP, cam);   // eye-image draws only (not shadow / reflection views)
                if (dc && !dc->haveVP) { dc->haveVP = true; dc->VP = VP; }
                add(true);
                if (!si.logged) {
                    si.logged = true;
                    static int nlog = 0;
                    if (nlog++ < 40)
                        LOGI("d3d11: VS %08X cb%d: ViewProjection at float4 %d%s, View %d%s, Projection %d%s, CameraPosition %d, ScreenToWorld %d (%d rows), "
                             "InvViewProjection %d, InvView %d; previous frame VP %d, View %d, Projection %d; camera %.0f %.0f %.0f, FOV %.1f x %.1f", hash, s, L.vp, L.vpT ? " (column order)" : "",
                             L.v, L.vS ? " (RH)" : "", L.p, L.pS ? " (RH)" : "",
                             L.cam, L.s2w, L.s2wN, L.ivp, L.iv, L.vp2, L.v2, L.p2, cam.pos[0], cam.pos[1], cam.pos[2], cam.fovH, cam.fovV);
                }
                continue;
            }
        }
        // no camera of its own: values derived from this frame's main camera (deferred lighting / fog in the PS)
        if (g_f.haveMain && (si.dtries <= 64 || (si.draws & 255) == 0)) {
            si.dtries++;
            ue3::CbLayout D;
            ue3::FindDerived(f4, n, g_f.mainVP, g_f.mainCam, &D, -1);
            if (D.any()) {
                L = D;
                LearnLayout(sh->bytes, L);
                add(false);
                static int nlog = 0;
                if (nlog++ < 40 || (!ps && nlog < 200))
                    LOGI("d3d11: %s %08X cb%d: derived camera values: CameraPosition %d (+%d), ScreenToWorld %d (%d rows), InvViewProjection %d, VP %d, "
                         "View %d, Projection %d, InvView %d; previous frame: VP %d (+%d), View %d, Projection %d",
                         ps ? "PS" : "VS", hash, s, L.cam, L.cam2, L.s2w, L.s2wN, L.ivp, L.vp, L.v, L.p, L.iv, L.vp2, L.vp2b, L.v2, L.p2);
            }
        }
    }
}
// 3D draws into the eye images (depth bound, eye-twinned target) whose shader has no camera found: these are drawn
// with the game's camera, so they stay fixed in the view when the head moves. Logged by vertex shader.
struct Unpatched { uint64_t draws = 0; uint32_t bytes[kCbSlots] = {}; int tries = 0; };
std::unordered_map<uint32_t, Unpatched> g_unp;   // (under g_mx)
std::string UnpatchedLine() {
    std::vector<std::pair<uint64_t, uint32_t>> v;
    for (auto& kv : g_unp) v.push_back({kv.second.draws, kv.first});
    std::sort(v.rbegin(), v.rend());
    std::string out;
    uint64_t total = 0;
    for (auto& e : v) total += e.first;
    char b[300];
    snprintf(b, sizeof b, "%llu draws from %zu vertex shaders", (unsigned long long)total, v.size());
    out = b;
    for (size_t i = 0; i < v.size() && i < 8; ++i) {
        Unpatched& u = g_unp[v[i].second];
        snprintf(b, sizeof b, " | VS %08X x%llu cb bytes", v[i].second, (unsigned long long)u.draws);
        out += b;
        for (int k = 0; k < kCbSlots; ++k) if (u.bytes[k]) { snprintf(b, sizeof b, " s%d=%u", k, u.bytes[k]); out += b; }
    }
    g_unp.clear();
    return out;
}
DrawCam ObserveDraw(Ctx11* x) {
    DrawCam dc;
    if (!x) return dc;
    { static bool pz = false; if (!pz) { pz = true; if (!G().s.debugLayoutPoison.empty()) g_poisonVs = (uint32_t)strtoul(G().s.debugLayoutPoison.c_str(), nullptr, 16); } }
    g_st.draws++;
    x->draws++;
    if (x->deferred) g_st.deferredDraws++;
    AddTimer at_(&g_pfObs);
    InterlockedIncrement64(&g_pfObsN);
    // draw_cam_cache (perf): the same shaders with the same, unchanged camera buffers as the previous draw on this
    // context give the same result - no lock, no search. Anything the check read (bound buffer, its contents version,
    // the frame, new buffers, this context's own copies) differing -> the full check below.
    Ctx11::DcKey& k = x->dck;
    long frame = G().frame;
    bool useCache = G().s.drawCamCache;
    if (useCache && k.valid && x->dcc && k.frame == frame && k.vs == x->curVS && k.ps == x->curPS && k.haveMain == g_f.haveMain &&
        k.cbGen == g_cbGen && k.cbSize == x->cb.size()) {
        bool same = true;
        for (int i = 0; i < k.n && same; ++i) {
            ID3D11Buffer* b = k.isPs[i] ? x->psCB[k.slot[i]] : x->vsCB[k.slot[i]];
            same = b == k.buf[i] && *(volatile uint32_t*)&k.sh[i]->ver == k.ver[i];
        }
        if (same) {
            InterlockedIncrement64(&g_pfObsHit);
            if (x->dcc->haveVP) { g_f.camDraws++; g_st.camDraws++; }
            return *x->dcc;
        }
    }
    k.valid = false;
    Ctx11::DcKey nk;
    nk.n = 0; nk.searched = false;
    MxGuard l;
    nk.frame = frame; nk.vs = x->curVS; nk.ps = x->curPS; nk.haveMain = g_f.haveMain; nk.cbGen = g_cbGen; nk.cbSize = x->cb.size();
    auto h = g_vsHash.find(x->curVS);
    if (h != g_vsHash.end()) {
        ShaderInfo& si = g_vsInfo[h->second];
        si.draws++;
        ObserveSlots(x, false, h->second, si, x->vsCB, &dc, useCache ? &nk : nullptr);
    } else nk.searched = true;
    if (g_f.haveMain && G().s.d3d11PatchPS) {
        auto p = g_psHash.find(x->curPS);
        if (p != g_psHash.end()) ObserveSlots(x, true, p->second, g_psInfo[p->second], x->psCB, &dc, useCache ? &nk : nullptr);
        else nk.searched = true;
    }
    if (useCache && !nk.searched && nk.haveMain == g_f.haveMain) {   // (SetMain during this check: not reused)
        if (!x->dcc) x->dcc = new DrawCam();
        *x->dcc = dc;
        nk.valid = true;
        k = nk;
    }
    if (dc.haveVP) g_f.camDraws++;
    if (!dc.n && g_f.active && x->b.dsv && (x->b.dsvT || x->b.rtvT[0]) && h != g_vsHash.end()) {
        Unpatched& u = g_unp[h->second];
        if (!u.draws++)
            for (int k = 0; k < kCbSlots; ++k) if (x->vsCB[k]) if (Shadow* sh = FindShadow(x, x->vsCB[k])) u.bytes[k] = sh->bytes;
    }
    return dc;
}

// Binds per-eye copies of the draw's camera constant buffers. Returns how many slots were replaced.
int PatchBind(ID3D11DeviceContext* c, Ctx11* x, int eye, const DrawCam& dc) {
    AddTimer at_(&g_pfPatch);
    // eye_mat_cache (perf): StereoEyeMats11 takes the global eye lock; per context the result for the last camera is
    // kept until the eye matrices change (next frame start) - two lock round trips less per stereo draw.
    EyeMats11 ownTmp;
    const EyeMats11* ownP = &ownTmp;
    bool ownOk = false;
    if (dc.haveVP) {
        if (G().s.eyeMatCache) {
            unsigned gen = StereoEyeGen11();
            if (x->emGen != gen || memcmp(x->emKey, &dc.VP.m[0][0], 64)) {
                x->emOk = StereoEyeMats11(&dc.VP.m[0][0], &x->em);
                x->emGen = gen; memcpy(x->emKey, &dc.VP.m[0][0], 64);
                InterlockedIncrement64(&g_pfEmCalc);
            } else InterlockedIncrement64(&g_pfEmHit);
            ownOk = x->emOk; ownP = &x->em;
        } else { ownOk = StereoEyeMats11(&dc.VP.m[0][0], &ownTmp); InterlockedIncrement64(&g_pfEmCalc); }
    }
    const EyeMats11& own = *ownP;
    int done = 0;
    // g_mx guards the shared shadow copies only: the per-eye buffers and the upload belong to this context (its thread)
    std::unique_lock<SpinMx> l(g_mx, std::defer_lock);
    for (int i = 0; i < dc.n; ++i) {
        const SlotPatch& sp = dc.s[i];
        const EyeMats11* em = sp.ownVP ? (ownOk ? &own : nullptr) : (g_f.mainEmOk ? &g_f.mainEm : nullptr);
        if (!em) continue;
        // snapshot of the shadow (bytes, version, contents) under the lock
        uint32_t shBytes, shVer;
        {
            Shadow* shp = nullptr;
            if (x->deferred && G().s.lockFree) { auto it = x->cb.find(sp.buf); if (it != x->cb.end() && it->second.gen == g_cbGen) shp = &it->second; }
            bool locked = false;
            if (!shp) { l.lock(); locked = true; shp = FindShadow(x, sp.buf); }
            if (!shp) { if (locked) l.unlock(); continue; }
            shBytes = shp->bytes; shVer = shp->ver;
            PatchBuf& pb0 = x->patch[sp.buf];
            bool need = pb0.bytes != shBytes || pb0.ver[eye] != shVer || pb0.frame[eye] != G().frame || memcmp(pb0.key[eye], em->VPe[eye], 64);
            if (need) x->scratch.assign(shp->f.begin(), shp->f.end());
            if (locked) l.unlock();
        }
        struct ShView { uint32_t bytes, ver; } sh{shBytes, shVer};
        PatchBuf& pb = x->patch[sp.buf];
        if (pb.bytes != sh.bytes) {
            for (int e = 0; e < 2; ++e) { if (pb.b[e]) pb.b[e]->Release(); pb.b[e] = nullptr; pb.frame[e] = -1; }
            pb.bytes = sh.bytes;
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = (sh.bytes + 15) & ~15u; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            for (int e = 0; e < 2; ++e) o_D11CreateBuffer(g_dev, &bd, nullptr, &pb.b[e]);
        }
        if (!pb.b[eye]) continue;
        const float* key = em->VPe[eye];
        if (pb.ver[eye] != sh.ver || pb.frame[eye] != G().frame || memcmp(pb.key[eye], key, 64)) {
            float* f4 = x->scratch.data();
            int n = (int)(x->scratch.size() / 4);
            const ue3::CbLayout& L = sp.L;
            auto put = [&](int r, const float* m, int rows, bool T) {
                if (r < 0 || r + rows > n) return;
                for (int a = 0; a < rows; ++a)
                    for (int b = 0; b < 4; ++b) f4[(r + a) * 4 + b] = T ? m[b * 4 + a] : m[a * 4 + b];
            };
            put(L.vp, em->VPe[eye], 4, L.vpT);
            if (L.cam >= 0 && L.cam < n) for (int k = 0; k < 3; ++k) f4[L.cam * 4 + k] = em->camE[eye][k];
            if (L.cam2 >= 0 && L.cam2 < n) for (int k = 0; k < 3; ++k) f4[L.cam2 * 4 + k] = em->camE[eye][k];
            if (L.iv >= 0) {   // inverse view (right-handed: z row negated)
                float m[16]; memcpy(m, em->iVe[eye], 64);
                if (L.ivS) for (int c2 = 0; c2 < 4; ++c2) m[8 + c2] = -m[8 + c2];
                put(L.iv, m, 4, L.ivT2);
            }
            put(L.s2w, em->QiVPe[eye], L.s2wN, false);
            put(L.ivp, em->iVPe[eye], 4, L.ivpT);
            if (L.v >= 0) {   // separate view matrix (right-handed games: z column negated)
                float m[16]; memcpy(m, em->Ve[eye], 64);
                if (L.vS) for (int r = 0; r < 4; ++r) m[r * 4 + 2] = -m[r * 4 + 2];
                put(L.v, m, 4, L.vT);
            }
            if (L.vp2 >= 0 && L.vp2 + 4 <= n) {   // previous frame: its own camera with the previous frame's eye transform
                ue3::M4 VP2 = ue3::LoadM4(x->scratch.data(), L.vp2, L.vp2T);   // (scratch = the shadow's contents)
                EyeMats11 em2Tmp;
                const EyeMats11* em2p = &em2Tmp;
                bool ok2;
                if (G().s.eyeMatCache) {
                    unsigned gen = StereoEyeGen11();
                    if (x->em2Gen != gen || x->em2Prev != G().s.prevFramePose || memcmp(x->em2Key, &VP2.m[0][0], 64)) {
                        x->em2Ok = StereoEyeMats11Prev(&VP2.m[0][0], &x->em2);
                        x->em2Gen = gen; x->em2Prev = G().s.prevFramePose; memcpy(x->em2Key, &VP2.m[0][0], 64);
                    }
                    ok2 = x->em2Ok; em2p = &x->em2;
                } else ok2 = StereoEyeMats11Prev(&VP2.m[0][0], &em2Tmp);
                const EyeMats11& em2 = *em2p;
                if (ok2) {
                    put(L.vp2, em2.VPe[eye], 4, L.vp2T);
                    if (L.vp2b >= 0 && G().s.prevVpCopy) put(L.vp2b, em2.VPe[eye], 4, L.vp2bT);
                    if (L.v2 >= 0) { float m[16]; memcpy(m, em2.Ve[eye], 64); if (L.v2S) for (int r = 0; r < 4; ++r) m[r * 4 + 2] = -m[r * 4 + 2]; put(L.v2, m, 4, L.v2T); }
                    if (L.p2 >= 0) { float m[16]; memcpy(m, em2.Pe[eye], 64); if (L.p2S) for (int c2 = 0; c2 < 4; ++c2) m[8 + c2] = -m[8 + c2]; put(L.p2, m, 4, L.p2T); }
                }
            }
            if (L.p >= 0) {   // separate projection (right-handed: z row negated)
                float m[16]; memcpy(m, em->Pe[eye], 64);
                if (L.pS) for (int c2 = 0; c2 < 4; ++c2) m[8 + c2] = -m[8 + c2];
                put(L.p, m, 4, L.pT);
            }
            OC(c, UpdateSubresource)(c, pb.b[eye], 0, nullptr, f4, 0, 0);
            pb.ver[eye] = sh.ver; pb.frame[eye] = G().frame; memcpy(pb.key[eye], key, 64);
        }
        if (sp.ps) OC(c, PSSetConstantBuffers)(c, sp.slot, 1, &pb.b[eye]);
        else { OC(c, VSSetConstantBuffers)(c, sp.slot, 1, &pb.b[eye]); BindOtherStages(c, x, x->vsCB[sp.slot], pb.b[eye]); }
        done++;
    }
    return done;
}
void RestoreCBs(ID3D11DeviceContext* c, Ctx11* x, const DrawCam& dc) {
    for (int i = 0; i < dc.n; ++i) {
        const SlotPatch& sp = dc.s[i];
        if (sp.ps) OC(c, PSSetConstantBuffers)(c, sp.slot, 1, &x->psCB[sp.slot]);
        else { OC(c, VSSetConstantBuffers)(c, sp.slot, 1, &x->vsCB[sp.slot]); BindOtherStages(c, x, x->vsCB[sp.slot], x->vsCB[sp.slot]); }
    }
}
void BindEye(ID3D11DeviceContext* c, Ctx11* x, bool right) {
    Bind11& g_b = x->b;
    ID3D11RenderTargetView* rt[8];
    for (UINT i = 0; i < g_b.nrtv; ++i) rt[i] = right && g_b.rtvT[i] ? g_b.rtvT[i] : g_b.rtv[i];
    OC(c, OMSetRenderTargets)(c, g_b.nrtv, rt, right && g_b.dsvT ? g_b.dsvT : g_b.dsv);
    if (g_b.nTwinSrv > 0) {
        ID3D11ShaderResourceView* sv[kSrv];
        for (UINT i = 0; i < g_b.maxSrv; ++i) sv[i] = right && g_b.srvT[i] ? g_b.srvT[i] : g_b.srv[i];
        OC(c, PSSetShaderResources)(c, 0, g_b.maxSrv, sv);
    }
}
// Draws `draw` into both eye textures (twinSrv: the right eye reads the twins), then restores the game's bindings.
template <class F> void DrawIntoEyes(ID3D11DeviceContext* c, Ctx11* x, F draw, bool twinSrv) {
    Bind11& g_b = x->b;
    ID3D11RenderTargetView* game = g_b.rtv[0];
    // HUD layer: UI textures read by the composite -> the HUD texture; the eye images get transparent black instead
    ID3D11ShaderResourceView* uiRep[kSrv] = {};   // eye images: UI slots read a blank texture in the UI's clear colour
    bool anyUi = false;
    if (twinSrv && G().s.d3d11HudLayer && g_hudTex)
        for (UINT i = 0; i < g_b.maxSrv && !anyUi; ++i)
            if (IsUiTexture(g_b.srv[i])) {
                ID3D11Resource* r = nullptr;
                g_b.srv[i]->GetResource(&r);
                if (!r) continue;
                D3D11_TEXTURE2D_DESC td{};
                ((ID3D11Texture2D*)r)->GetDesc(&td);
                float cc[4] = {0, 0, 0, 0};
                { std::lock_guard<std::mutex> l(g_rtUseMx); auto it = g_clearCol.find(r); if (it != g_clearCol.end()) memcpy(cc, it->second.data(), 16); }
                bool inverted = cc[3] > 0.5f;   // cleared to alpha 1: alpha = how much of the scene shows through
                uiRep[i] = UiBlank(g_b.srv[i], r, cc);
                if (uiRep[i]) {
                    anyUi = true;
                    {   // the UI texture can be larger than the screen (title: 4096x2304): the conversion pass scales it
                        ID3D11RenderTargetView* hv = HudView(g_b.rtv[0]);
                        if (inverted && hv) ConvertUiToHud(c, g_b.srv[i], hv, g_eyeW, g_eyeH);
                        else if (td.Width == g_eyeW && td.Height == g_eyeH && Typeless(td.Format) == g_eyeFmt) OC(c, CopySubresourceRegion)(c, g_hudTex, 0, 0, 0, 0, r, 0, nullptr);
                        g_f.hud++;
                        g_f.uiTex = true;
                    }
                    static int logged = 0;
                    if (logged++ < 2 || (G().frame % 600) == 0) LOGI("d3d11 stereo: composite reads a UI texture %ux%u format %d in slot %u (clear %.2f %.2f %.2f %.2f%s) -> HUD layer",
                                           td.Width, td.Height, (int)td.Format, i, cc[0], cc[1], cc[2], cc[3], inverted ? ", alpha inverted" : "");
                }
                r->Release();
            }
    for (int e = 0; e < 2; ++e) {
        ID3D11RenderTargetView* v = EyeView(e, game);
        if (!v) continue;
        OC(c, OMSetRenderTargets)(c, 1, &v, nullptr);
        if (twinSrv && (g_b.nTwinSrv > 0 || anyUi)) {
            ID3D11ShaderResourceView* sv[kSrv];
            for (UINT i = 0; i < g_b.maxSrv; ++i) sv[i] = uiRep[i] ? uiRep[i] : e == 1 && g_b.srvT[i] ? g_b.srvT[i] : g_b.srv[i];
            OC(c, PSSetShaderResources)(c, 0, g_b.maxSrv, sv);
        }
        draw();
    }
    OC(c, OMSetRenderTargets)(c, g_b.nrtv, g_b.rtv, g_b.dsv);
    if (twinSrv && (g_b.nTwinSrv > 0 || anyUi)) OC(c, PSSetShaderResources)(c, 0, g_b.maxSrv, g_b.srv);
}
ID3D11RenderTargetView* HudView(ID3D11RenderTargetView* game) {
    if (!g_hudTex) return nullptr;
    D3D11_RENDER_TARGET_VIEW_DESC gd{};
    game->GetDesc(&gd);
    DXGI_FORMAT f = gd.Format == DXGI_FORMAT_B8G8R8X8_UNORM ? DXGI_FORMAT_B8G8R8A8_UNORM : gd.Format;
    for (auto& x : g_hudRtv) if (x.f == f) return x.v;
    HudRtv x{f, nullptr};
    D3D11_RENDER_TARGET_VIEW_DESC vd{};
    vd.Format = f; vd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (FAILED(o_D11CreateRenderTargetView(g_dev, g_hudTex, &vd, &x.v))) x.v = nullptr;
    g_hudRtv.push_back(x);
    return x.v;
}
void ClearHud(ID3D11DeviceContext* c, Ctx11* x) {
    Bind11& g_b = x->b;
    const float zero[4] = {0, 0, 0, 0};
    for (auto& x : g_hudRtv) if (x.v) OC(c, ClearRenderTargetView)(c, x.v, zero);
    if (g_hudRtv.empty() && g_b.rtv[0]) if (auto v = HudView(g_b.rtv[0])) OC(c, ClearRenderTargetView)(c, v, zero);
}
// UI draw repeated into the HUD texture; with blending, alpha accumulates as a = a_src + a_dst * (1 - a_src) so the
// result is premultiplied over transparent (the D3D9 HUD layer does the same).
template <class F> void DrawIntoHud(ID3D11DeviceContext* c, Ctx11* x, F draw) {
    Bind11& g_b = x->b;
    ID3D11RenderTargetView* v = HudView(g_b.rtv[0]);
    if (!v) return;
    ID3D11BlendState* bs = nullptr;
    FLOAT bf[4]; UINT mask = 0;
    c->OMGetBlendState(&bs, bf, &mask);
    ID3D11BlendState* hb = nullptr;
    if (bs) {
        auto it = g_hudBlend.find(bs);
        if (it == g_hudBlend.end()) {
            D3D11_BLEND_DESC d{};
            bs->GetDesc(&d);
            if (d.RenderTarget[0].BlendEnable) {
                d.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
                d.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
                d.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
                d.RenderTarget[0].RenderTargetWriteMask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
                if (FAILED(g_dev->CreateBlendState(&d, &hb))) hb = nullptr;
            }
            it = g_hudBlend.emplace(bs, hb).first;
            bs->AddRef();   // the key stays alive (no address reuse); released in ReleaseEyes
        }
        hb = it->second;
    }
    OC(c, OMSetRenderTargets)(c, 1, &v, nullptr);
    if (hb) c->OMSetBlendState(hb, bf, mask);
    draw();
    if (hb) c->OMSetBlendState(bs, bf, mask);
    OC(c, OMSetRenderTargets)(c, g_b.nrtv, g_b.rtv, g_b.dsv);
    if (bs) bs->Release();
}

// The composite (first back-buffer pass reading the 3D scene): decides whether this frame is shown in stereo.
bool CompositeStarts(ID3D11DeviceContext* c, Ctx11* x) {
    Trace("f%ld composite check: composited %d active %d eyeW %u camDraws %u patch %d", G().frame, (int)g_f.composited, (int)g_f.active, g_eyeW, g_f.camDraws, (int)g_f.patch);
    if (g_f.composited || !g_f.active || g_eyeW == 0) return false;
    if (g_f.camDraws < (unsigned)G().s.stereoMinDraws) return false;   // menus, title backdrop: flat screen
    if (!g_f.patch) { g_want11 = true; return false; }                   // drawn with the game's camera: next frame
    g_f.composited = true;
    if (G().s.d3d11HudLayer) ClearHud(c, x);
    StereoEyesCommit11(g_f.mainEmOk ? g_f.mainEm.P00 : 0, g_f.mainEmOk ? g_f.mainEm.P11 : 0);
    static int logged = 0;
    if (logged++ < 3) LOGI("d3d11 stereo: composite at frame %ld (%u camera draws, %u patched, %u right-eye draws) -> eye images", G().frame,
                           g_f.camDraws, g_f.patched, g_f.right);
    return true;
}

// UI textures: render targets that only ever receive draws without a camera (GreedFall draws its whole HUD into one
// such texture and blends it over the scene in the final composite). With the HUD layer on, the composite's eye
// images get no UI and the texture itself becomes the HUD panel.
struct RtUse { uint32_t cam = 0, nocam = 0; long lastCam = -100000; };
std::mutex g_rtUseMx;
std::unordered_map<ID3D11Resource*, RtUse> g_rtUse;
std::unordered_set<ID3D11Resource*> g_uiKnown;   // targets recognised as the game's UI texture (under g_rtUseMx)
void NoteRtUse(Ctx11* x, bool cam) {
    if (!x->b.rtv[0] || !x->b.rtvT[0]) return;
    ID3D11Resource* r = nullptr;
    x->b.rtv[0]->GetResource(&r);
    if (!r) return;
    { std::lock_guard<std::mutex> l(g_rtUseMx); RtUse& u = g_rtUse[r]; if (cam) { u.cam++; u.lastCam = G().frame; } else if (u.nocam < 1000000) u.nocam++; }
    r->Release();
}
std::unordered_map<ID3D11Resource*, std::array<float, 4>> g_clearCol;   // last clear colour of twinned targets
void NoteClear(ID3D11RenderTargetView* v, const FLOAT* col) {
    if (!v || !col) return;
    ID3D11Resource* r = nullptr;
    v->GetResource(&r);
    if (!r) return;
    { std::lock_guard<std::mutex> l(g_rtUseMx); g_clearCol[r] = {col[0], col[1], col[2], col[3]}; }
    r->Release();
}
// blank stand-in for the UI texture in the eye images (filled with the UI's clear colour) and the HUD conversion pass
ID3D11Texture2D* g_uiBlank = nullptr; ID3D11ShaderResourceView* g_uiBlankSrv = nullptr; ID3D11RenderTargetView* g_uiBlankRtv = nullptr;
DXGI_FORMAT g_uiBlankFmt = DXGI_FORMAT_UNKNOWN; UINT g_uiBlankW = 0, g_uiBlankH = 0;
ID3D11VertexShader* g_fsVS = nullptr; ID3D11PixelShader* g_invPS = nullptr; ID3D11SamplerState* g_linSamp = nullptr; bool g_fsFailed = false;
ID3D11ShaderResourceView* UiBlank(ID3D11ShaderResourceView* ui, ID3D11Resource* r, const float* cc) {
    D3D11_TEXTURE2D_DESC td{}; ((ID3D11Texture2D*)r)->GetDesc(&td);
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{}; ui->GetDesc(&sd);
    if (!g_uiBlank || g_uiBlankW != td.Width || g_uiBlankH != td.Height || g_uiBlankFmt != sd.Format) {
        if (g_uiBlankSrv) g_uiBlankSrv->Release(); if (g_uiBlankRtv) g_uiBlankRtv->Release(); if (g_uiBlank) g_uiBlank->Release();
        g_uiBlank = nullptr; g_uiBlankSrv = nullptr; g_uiBlankRtv = nullptr;
        D3D11_TEXTURE2D_DESC d{}; d.Width = td.Width; d.Height = td.Height; d.MipLevels = 1; d.ArraySize = 1; d.Format = td.Format;
        d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        Bypass11 bp;
        if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_uiBlank))) return nullptr;
        D3D11_SHADER_RESOURCE_VIEW_DESC s2 = sd; s2.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; s2.Texture2D.MostDetailedMip = 0; s2.Texture2D.MipLevels = 1;
        D3D11_RENDER_TARGET_VIEW_DESC rd{}; rd.Format = sd.Format; rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(g_dev->CreateShaderResourceView(g_uiBlank, &s2, &g_uiBlankSrv)) || FAILED(g_dev->CreateRenderTargetView(g_uiBlank, &rd, &g_uiBlankRtv))) return nullptr;
        g_uiBlankW = td.Width; g_uiBlankH = td.Height; g_uiBlankFmt = sd.Format;
    }
    OC(g_imm, ClearRenderTargetView)(g_imm, g_uiBlankRtv, cc);
    return g_uiBlankSrv;
}
bool EnsureFsShaders() {
    if (g_fsVS && g_invPS) return true;
    if (g_fsFailed) return false;
    g_fsFailed = true;
    typedef HRESULT(WINAPI* Compile_t)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    HMODULE m = LoadLibraryA("d3dcompiler_47.dll");
    Compile_t comp = m ? (Compile_t)GetProcAddress(m, "D3DCompile") : nullptr;
    if (!comp) { LOGW("d3d11: d3dcompiler_47 not available - HUD alpha conversion off"); return false; }
    const char* vs = "void main(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) { uv = float2((id << 1) & 2, id & 2); pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }";
    const char* ps = "Texture2D t : register(t0); SamplerState s : register(s0); float4 main(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target { float4 c = t.Sample(s, uv); return float4(c.rgb, saturate(1 - c.a)); }";
    ID3DBlob *bv = nullptr, *bpx = nullptr, *err = nullptr;
    if (FAILED(comp(vs, strlen(vs), nullptr, nullptr, nullptr, "main", "vs_4_0", 0, 0, &bv, &err)) ||
        FAILED(comp(ps, strlen(ps), nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, &bpx, &err))) {
        LOGW("d3d11: HUD shader compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        if (err) err->Release(); if (bv) bv->Release(); if (bpx) bpx->Release();
        return false;
    }
    Bypass11 bp;
    bool ok = SUCCEEDED(g_dev->CreateVertexShader(bv->GetBufferPointer(), bv->GetBufferSize(), nullptr, &g_fsVS)) &&
              SUCCEEDED(g_dev->CreatePixelShader(bpx->GetBufferPointer(), bpx->GetBufferSize(), nullptr, &g_invPS));
    if (ok) {
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
        ok = SUCCEEDED(g_dev->CreateSamplerState(&sd, &g_linSamp));
    }
    bv->Release(); bpx->Release();
    g_fsFailed = !ok;
    LOGI("d3d11: HUD alpha conversion shaders %s", ok ? "ready" : "FAILED");
    return ok;
}
// HUD texture = UI texture with alpha inverted (the game's UI stores 'scene shows through' in alpha)
// ---- VRS foveation (NVIDIA variable rate shading, NVAPI without the SDK lib; IDs and approach from GranblueVR vrs.cpp,
//      sharedtools\vr-mod-toolkit.md). One shading-rate image per eye (R8_UINT, one texel per 16x16 pixel tile) indexes a
//      per-viewport table: 0 = full rate, 1 = middle ring, 2 = outer ring. The rings are centred on each eye's optical axis
//      (the eye projections are asymmetric). Applied only to the game's draws into eye-size targets that have a right-eye twin
//      (scene, lights, full-size effects); off for shadow maps, half-res passes, the composite, UI and every pass of the mod.
//      The state is cached per context (NVAPI calls only on change) and forgotten at command-list boundaries.
namespace vrs {
typedef void* (__cdecl* QI_t)(unsigned);
typedef int (__cdecl* Init_t)();
struct ViewportDesc { bool enable; int table[16]; };
struct ViewportsDesc { unsigned version; unsigned num; ViewportDesc* vp; };
struct SrrvDesc { unsigned version; DXGI_FORMAT format; int dim; unsigned mip, first, size; };
typedef int (__cdecl* CreateSrrv_t)(ID3D11Device*, ID3D11Resource*, SrrvDesc*, IUnknown**);
typedef int (__cdecl* SetSrrv_t)(IUnknown*, IUnknown*);
typedef int (__cdecl* SetRates_t)(IUnknown*, ViewportsDesc*);
static CreateSrrv_t g_create = nullptr; static SetSrrv_t g_setView = nullptr; static SetRates_t g_setRates = nullptr;
static int g_state = 0;   // 0 not tried, 1 ready, -1 unavailable
static ID3D11Texture2D* g_tex[2] = {}; static IUnknown* g_view[2] = {};
static UINT g_tw = 0, g_th = 0; static char g_key[128] = {}; static IUnknown* g_sv1 = nullptr;   // right view while parked
static volatile LONG64 g_draws = 0, g_drawsAll = 0;
static SRWLOCK g_nvLock = SRWLOCK_INIT;   // NVAPI is not safe to call from several recording threads at once (test 111: crash in nvapi64_impl)
struct NvGuard { NvGuard() { AcquireSRWLockExclusive(&g_nvLock); } ~NvGuard() { ReleaseSRWLockExclusive(&g_nvLock); } };
enum { kX1 = 5, kX1_1x2 = 7, kX1_2x2 = 8, kX1_4x4 = 11 };   // NV_PIXEL_SHADING_RATE
bool Init() {
    if (g_state) return g_state > 0;
    g_state = -1;
    HMODULE m = LoadLibraryA("nvapi64.dll");
    QI_t qi = m ? (QI_t)GetProcAddress(m, "nvapi_QueryInterface") : nullptr;
    if (!qi) { LOGI("vrs: NVAPI not available (not an NVIDIA GPU?) - foveated rendering off"); return false; }
    Init_t init = (Init_t)qi(0x0150e828);
    g_create = (CreateSrrv_t)qi(0x99ca2dff); g_setView = (SetSrrv_t)qi(0x1b0c2f83); g_setRates = (SetRates_t)qi(0x34f7938f);
    int r = init ? init() : -1;
    if (r != 0 || !g_create || !g_setView || !g_setRates) {
        LOGW("vrs: NVAPI init %d, functions %d%d%d - foveated rendering off", r, (int)!!g_create, (int)!!g_setView, (int)!!g_setRates);
        return false;
    }
    g_state = 1;
    LOGI("vrs: NVAPI variable rate shading ready");
    return true;
}
bool Ready() { return g_state > 0 && g_view[0] && g_view[1]; }
// rates of the middle / outer ring per strength: 1 gentle, 2 balanced, 3 strong
static void Rates(int strength, int& mid, int& out) {
    if (strength <= 1) { mid = kX1_1x2; out = kX1_2x2; }
    else if (strength == 2) { mid = kX1_2x2; out = kX1_2x2; }
    else { mid = kX1_2x2; out = kX1_4x4; }
}
void Set(ID3D11DeviceContext* c, Ctx11* x, int want) {   // want: 0 off, 1 left, 2 right
    if (!x || g_state <= 0) return;
    if (want && !Ready()) want = 0;
    if (x->vrsState == want) return;
    if (want && x->vrsState > 0) {   // eye to eye: only the image changes (none at all with one shared image)
        if (g_view[want - 1] != g_view[x->vrsState - 1]) { NvGuard g; g_setView(c, g_view[want - 1]); }
        x->vrsState = want;
        return;
    }
    ViewportDesc vd{}; ViewportsDesc d{(unsigned)sizeof(ViewportsDesc) | (1u << 16), 1, &vd};
    vd.enable = want != 0;
    int mid, out; Rates(G().s.vrsStrength, mid, out);
    for (int i = 0; i < 16; ++i) vd.table[i] = kX1;
    vd.table[1] = mid; vd.table[2] = out;
    { NvGuard g; if (want) g_setView(c, g_view[want - 1]); g_setRates(c, &d); }
    x->vrsState = want;
}
// Per frame on the immediate context (Present): (re)build the per-eye images when the eye size, eye projection or settings change.
void Frame(ID3D11Device* dev, ID3D11DeviceContext* imm, UINT eyeW, UINT eyeH, const float* Pl, const float* Pr) {
    if (G().s.vrsStrength <= 0) xrb::XrSetEyeTracking(false);
    if (G().s.vrsStrength <= 0 || !eyeW || !dev || !Init()) return;
    UINT tw = (eyeW + 15) / 16, th = (eyeH + 15) / 16;
    float cx[2], cy[2];
    const float* P[2] = {Pl, Pr};
    for (int e = 0; e < 2; ++e) {   // optical axis in pixels: NDC (P20/P23, P21/P23), image y points down
        float ox = P[e] && P[e][11] != 0 ? P[e][8] / P[e][11] : 0.f, oy = P[e] && P[e][11] != 0 ? P[e][9] / P[e][11] : 0.f;
        cx[e] = (ox + 1.f) * 0.5f * eyeW; cy[e] = (1.f - oy) * 0.5f * eyeH;
    }
    // eye tracking: where the gaze meets each eye image (the poses / FOVs the next frame is rendered with)
    bool gaze = false;
    xrb::XrSetEyeTracking(G().s.vrsEyeTracking);
    xrb::XrSetSimulatedGaze(G().s.simGaze, 3.f);
    float gd[3];
    xrb::IpcPose ep[2]; xrb::IpcFov ef[2]; int64_t et = 0;
    if (G().s.vrsEyeTracking && xrb::XrGetGaze(gd) && xrb::ClientGetLatestPoses(ep, ef, &et)) {
        gaze = true;
        for (int e = 0; e < 2 && gaze; ++e) {
            float qx = -ep[e].qx, qy = -ep[e].qy, qz = -ep[e].qz, qw = ep[e].qw;   // conjugate: ref space -> eye space
            float tx = 2 * (qy * gd[2] - qz * gd[1]), ty = 2 * (qz * gd[0] - qx * gd[2]), tz = 2 * (qx * gd[1] - qy * gd[0]);
            float d[3] = {gd[0] + qw * tx + (qy * tz - qz * ty), gd[1] + qw * ty + (qz * tx - qx * tz), gd[2] + qw * tz + (qx * ty - qy * tx)};
            if (d[2] > -0.05f) { gaze = false; break; }
            float ax = d[0] / -d[2], ay = d[1] / -d[2];
            float tl = tanf(ef[e].left), tr = tanf(ef[e].right), tu = tanf(ef[e].up), tdn = tanf(ef[e].down);
            if (tr - tl < 1e-4f || tu - tdn < 1e-4f) { gaze = false; break; }
            cx[e] = std::min(std::max((ax - tl) / (tr - tl), 0.f), 1.f) * eyeW;
            cy[e] = std::min(std::max((tu - ay) / (tu - tdn), 0.f), 1.f) * eyeH;
        }
        if (!gaze) for (int e = 0; e < 2; ++e) {   // back to the lens centres
            float ox = P[e] && P[e][11] != 0 ? P[e][8] / P[e][11] : 0.f, oy = P[e] && P[e][11] != 0 ? P[e][9] / P[e][11] : 0.f;
            cx[e] = (ox + 1.f) * 0.5f * eyeW; cy[e] = (1.f - oy) * 0.5f * eyeH;
        }
    }
    static bool lastGaze = false;
    if (gaze != lastGaze) { lastGaze = gaze; LOGI("vrs: full-rate area %s", gaze ? "follows the eye tracker" : "at the lens centres"); }
    static float lc[4] = {-1e9f, -1e9f, -1e9f, -1e9f};   // centres used for the current images: rebuilt only if one moved > 2 tiles (gaze: half a tile)
    float now[4] = {cx[0], cy[0], cx[1], cy[1]};
    bool moved = false;
    for (int i = 0; i < 4; ++i) moved |= fabsf(now[i] - lc[i]) > (gaze ? 8.f : 32.f);
    if (moved) memcpy(lc, now, sizeof lc); else { cx[0] = lc[0]; cy[0] = lc[1]; cx[1] = lc[2]; cy[1] = lc[3]; }
    char key[128];
    snprintf(key, sizeof key, "%u %u %d %.2f %.2f %d %d %d %d %d", eyeW, eyeH, G().s.vrsStrength, G().s.vrsInner, G().s.vrsOuter, (int)G().s.vrsPerEye,
             (int)lc[0], (int)lc[1], (int)lc[2], (int)lc[3]);
    if (Ready() && g_tw == tw && g_th == th && !strcmp(key, g_key)) return;
    Bypass11 bp;
    if (g_tw != tw || g_th != th || !g_tex[0]) {
        if (g_sv1) { g_view[1] = g_sv1; g_sv1 = nullptr; }
        for (int e = 0; e < 2; ++e) { if (g_view[e]) g_view[e]->Release(); if (g_tex[e]) g_tex[e]->Release(); g_view[e] = nullptr; g_tex[e] = nullptr; }
        D3D11_TEXTURE2D_DESC td{}; td.Width = tw; td.Height = th; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8_UINT;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        for (int e = 0; e < 2; ++e) {
            if (FAILED(dev->CreateTexture2D(&td, nullptr, &g_tex[e]))) { LOGW("vrs: shading-rate texture failed - foveated rendering off"); g_state = -1; return; }
            SrrvDesc sd{(unsigned)sizeof(SrrvDesc) | (1u << 16), DXGI_FORMAT_R8_UINT, 4 /* TEXTURE2D */, 0, 0, 0};
            int r; { NvGuard g; r = g_create(dev, g_tex[e], &sd, &g_view[e]); }
            if (r != 0 || !g_view[e]) { LOGW("vrs: CreateShadingRateResourceView failed (%d) - foveated rendering off", r); g_state = -1; return; }
        }
        g_tw = tw; g_th = th;
    }
    // rings by distance from the optical axis, in units of half the image height
    std::vector<uint8_t> img(tw * th);
    float r1 = G().s.vrsInner, r2 = std::max(G().s.vrsOuter, r1 + 0.01f), half = eyeH * 0.5f, r1q = r1 * r1, r2q = r2 * r2;
    bool perEye = G().s.vrsPerEye;
    int full = 0, mid = 0;
    for (int e = 0; e < 2; ++e) {
        for (UINT y = 0; y < th; ++y) for (UINT xx = 0; xx < tw; ++xx) {
            float r = 1e9f;   // squared; per-eye centres, or (default) one image for both eyes: the nearer of the two centres
            for (int k = 0; k < 2; ++k) {
                if (perEye && k != e) continue;
                float dx = ((xx + 0.5f) * 16 - cx[k]) / half, dy = ((y + 0.5f) * 16 - cy[k]) / half;
                r = std::min(r, dx * dx + dy * dy);
            }
            uint8_t v = r < r1q ? 0 : (r < r2q ? 1 : 2);
            img[y * tw + xx] = v;
            if (e == 0) { full += v == 0; mid += v == 1; }
        }
        imm->UpdateSubresource(g_tex[e], 0, nullptr, img.data(), tw, 0);
        if (!G().s.vrsPerEye) break;
    }
    // one shared image: both eyes use view 0 (no state change between the two draws of a stereo pair)
    if (!G().s.vrsPerEye) { if (g_view[1] != g_view[0]) { g_sv1 = g_view[1]; g_view[1] = g_view[0]; } }
    else if (g_sv1) { g_view[1] = g_sv1; g_sv1 = nullptr; }
    strcpy(g_key, key);
    static const char* nm[] = {"1x1", "1x2", "2x2", "4x4"};
    int m, o; Rates(G().s.vrsStrength, m, o);
    static long nLog = 0; static DWORD lastLog = 0;
    if (gaze && nLog > 3 && GetTickCount() - lastLog < 5000) return;
    ++nLog; lastLog = GetTickCount();
    LOGI("vrs: %ux%u tiles per eye (%ux%u px): full rate inside %.2f (%.0f%% of tiles), %s to %.2f (%.0f%%), %s outside; eye centres L %.0f,%.0f R %.0f,%.0f px",
         tw, th, eyeW, eyeH, r1, 100.0 * full / (tw * th), nm[m == kX1_1x2 ? 1 : 2], r2, 100.0 * mid / (tw * th), nm[o == kX1_4x4 ? 3 : 2], cx[0], cy[0], cx[1], cy[1]);
}
}  // namespace vrs
void VrsOffForMod(ID3D11DeviceContext* c) { if (vrs::g_state > 0) vrs::Set(c, X(c), 0); }

void ConvertUiToHud(ID3D11DeviceContext* c, ID3D11ShaderResourceView* ui, ID3D11RenderTargetView* hud, UINT w, UINT h) {
    if (!EnsureFsShaders()) return;
    VrsOffForMod(c);
    Bypass11 bp;
    ID3D11VertexShader* vs = nullptr; ID3D11PixelShader* ps = nullptr; ID3D11InputLayout* il = nullptr; D3D11_PRIMITIVE_TOPOLOGY topo;
    ID3D11BlendState* bs = nullptr; FLOAT bf[4]; UINT mask = 0; ID3D11DepthStencilState* ds = nullptr; UINT sref = 0; ID3D11RasterizerState* rs = nullptr;
    UINT nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE; D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    ID3D11ShaderResourceView* srv0 = nullptr;
    ID3D11SamplerState* smp0 = nullptr;
    c->PSGetSamplers(0, 1, &smp0);
    c->VSGetShader(&vs, nullptr, nullptr); c->PSGetShader(&ps, nullptr, nullptr); c->IAGetInputLayout(&il); c->IAGetPrimitiveTopology(&topo);
    c->OMGetBlendState(&bs, bf, &mask); c->OMGetDepthStencilState(&ds, &sref); c->RSGetState(&rs); c->RSGetViewports(&nvp, vps); c->PSGetShaderResources(0, 1, &srv0);
    D3D11_VIEWPORT v{0, 0, (float)w, (float)h, 0, 1};
    c->OMSetRenderTargets(1, &hud, nullptr);
    c->VSSetShader(g_fsVS, nullptr, 0); c->PSSetShader(g_invPS, nullptr, 0); c->IASetInputLayout(nullptr);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); c->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    c->OMSetDepthStencilState(nullptr, 0); c->RSSetState(nullptr); c->RSSetViewports(1, &v); c->PSSetShaderResources(0, 1, &ui); c->PSSetSamplers(0, 1, &g_linSamp);
    c->Draw(3, 0);
    c->VSSetShader(vs, nullptr, 0); c->PSSetShader(ps, nullptr, 0); c->IASetInputLayout(il); c->IASetPrimitiveTopology(topo);
    c->OMSetBlendState(bs, bf, mask); c->OMSetDepthStencilState(ds, sref); c->RSSetState(rs); c->RSSetViewports(nvp, vps); c->PSSetShaderResources(0, 1, &srv0); c->PSSetSamplers(0, 1, &smp0); if (smp0) smp0->Release();
    if (vs) vs->Release(); if (ps) ps->Release(); if (il) il->Release(); if (bs) bs->Release(); if (ds) ds->Release(); if (rs) rs->Release(); if (srv0) srv0->Release();
}
bool IsUiTexture(ID3D11ShaderResourceView* v) {
    if (!v) return false;
    ID3D11Resource* r = nullptr;
    v->GetResource(&r);
    if (!r) return false;
    // Sticky: GreedFall also draws some 3D items (markers) into its UI texture in gameplay; those camera draws must not
    // turn it back into a scene texture (it then went into both eyes at the top edge, head-locked - user, build 11).
    // A texture recognised once stays UI; so does a later texture with the same size and format (targets recreated).
    auto& known = g_uiKnown; static std::vector<std::array<UINT, 3>> knownDesc;
    D3D11_TEXTURE2D_DESC td{}; ((ID3D11Texture2D*)r)->GetDesc(&td);
    bool ui = false;
    {
        std::lock_guard<std::mutex> l(g_rtUseMx);
        auto it = g_rtUse.find(r);
        if (known.count(r)) ui = true;
        // cam == 0, or no camera draw for 120 frames: GreedFall's small menus (Back / equipment, user build 15) draw their UI
        // into a screen-size target from a pool that earlier held a scene pass -> its old camera draws kept it out of the HUD.
        else if (it != g_rtUse.end() && it->second.nocam >= 30 && (it->second.cam == 0 || G().frame - it->second.lastCam > 120)) {
            ui = true;
            if (it->second.cam == 0) known.insert(r);   // a recycled target stays decided per use (not sticky)
            else { static std::unordered_set<ID3D11Resource*> said; if (said.insert(r).second && said.size() < 20) LOGI("d3d11 stereo: %ux%u target with old camera draws (last %ld frames ago) now only UI -> HUD layer", td.Width, td.Height, G().frame - it->second.lastCam); }
            bool have = false; for (auto& d : knownDesc) have |= d[0] == td.Width && d[1] == td.Height && d[2] == (UINT)td.Format;
            if (!have && td.Width > g_eyeW && td.Height > g_eyeH) knownDesc.push_back({td.Width, td.Height, (UINT)td.Format});   // larger than the screen: only UI does that here
        } else if (it != g_rtUse.end() && it->second.nocam >= 30)
            for (auto& d : knownDesc) if (d[0] == td.Width && d[1] == td.Height && d[2] == (UINT)td.Format) { ui = true; known.insert(r); break; }
    }
    r->Release();
    return ui;
}

// Test aid (hide_vs_cycle = "HASH,HASH,..."): every hide_vs_seconds one vertex shader of the list is not drawn (logged
// when it changes); screenshots then show what each shader draws. Phase 0 of each cycle hides nothing.
static std::vector<uint32_t> g_hideList;
static volatile uint32_t g_hideNow = 0;
void HideCycleTick() {
    static bool parsed = false;
    if (!parsed) {
        parsed = true;
        const std::string& s = G().s.hideVsCycle;
        size_t i = s == "auto" ? s.size() : 0;
        while (i < s.size()) {
            size_t j = s.find(',', i); if (j == std::string::npos) j = s.size();
            std::string t = s.substr(i, j - i);
            if (!t.empty()) g_hideList.push_back((uint32_t)strtoul(t.c_str(), nullptr, 16));
            i = j + 1;
        }
        if (!g_hideList.empty()) LOGI("d3d11: hide_vs_cycle with %zu shaders, %.1f s each", g_hideList.size(), G().s.hideVsSeconds);
    }
    // "auto": every vertex shader that drew into the eye images with a camera (collected when the cycle starts), one
    // atlas per shader in the middle of its slot
    bool autoMode = G().s.hideVsCycle == "auto";
    if (autoMode && g_hideList.empty() && SecondsSinceStart() >= G().s.hideVsStartSec) {
        { MxGuard l; for (auto& kv : g_vsInfo) if (kv.second.camDraws > 50) g_hideList.push_back(kv.first); }
        std::sort(g_hideList.begin(), g_hideList.end());
        std::string all; char b[12]; for (auto h : g_hideList) { snprintf(b, sizeof b, " %08X", h); all += b; }
        LOGI("d3d11: hide_vs_cycle auto: %zu camera shaders, %.1f s each:%s", g_hideList.size(), G().s.hideVsSeconds, all.c_str());
    }
    if (g_hideList.empty() || SecondsSinceStart() < G().s.hideVsStartSec) return;
    double inCycle = (SecondsSinceStart() - G().s.hideVsStartSec) / G().s.hideVsSeconds;
    int slot = (int)inCycle % (int)(g_hideList.size() + 1);
    if (autoMode && (int)inCycle > (int)g_hideList.size()) { if (g_hideNow) { g_hideNow = 0; LOGI("d3d11: hide cycle done"); } return; }
    uint32_t h = slot == 0 ? 0 : g_hideList[slot - 1];
    if (h != g_hideNow || (slot == 0 && inCycle < 0.01)) { g_hideNow = h; LOGI("d3d11: hiding VS %08X from frame %ld (slot %d)", h, G().frame, slot); }
    static int snapped = -1;
    if (autoMode && inCycle - (int)inCycle > 0.6 && snapped != (int)inCycle) { snapped = (int)inCycle; g_snapLeft = 1; g_snapNext = 0; }
}
static uint32_t g_isoVs = 0;
// Debug "Hide effect pass": every full-screen pass (non-indexed draw of <= 4 vertices into a screen-sized eye target or
// the back buffer) is listed once by pixel shader, in the order first seen; the menu picks one to skip (both eyes).
// For the user to find the post effect behind the head-locked "haze" by eye (build 16).
struct FxPass { uint32_t ps; UINT w, h; DXGI_FORMAT f; int srvs; bool cam; long first; };
static std::mutex g_fxMx; static std::vector<FxPass> g_fx; static std::unordered_set<uint32_t> g_fxSeen;
static volatile uint32_t g_fxHidePs = 0;
static void NoteFxPass(Ctx11* x, uint32_t ps, bool cam) {
    if (!ps || !x->b.rtv[0] || (!x->b.rtvT[0] && !x->b.rt0IsBB)) return;
    { std::lock_guard<std::mutex> l(g_fxMx); if (g_fxSeen.count(ps) || g_fx.size() >= 120) return; }
    ID3D11Resource* r = nullptr; x->b.rtv[0]->GetResource(&r); if (!r) return;
    D3D11_TEXTURE2D_DESC td{}; ((ID3D11Texture2D*)r)->GetDesc(&td); r->Release();
    int n = 0; for (UINT i = 0; i < x->b.maxSrv; ++i) n += x->b.srv[i] ? 1 : 0;
    std::lock_guard<std::mutex> l(g_fxMx);
    if (!g_fxSeen.insert(ps).second) return;
    g_fx.push_back({ps, td.Width, td.Height, td.Format, n, cam, G().frame});
    LOGI("d3d11: effect pass %zu: PS %08X -> %ux%u format %d, %d textures%s", g_fx.size(), ps, td.Width, td.Height, (int)td.Format, n, cam ? ", camera" : "");
}
int FxPassCount() { std::lock_guard<std::mutex> l(g_fxMx); return (int)g_fx.size(); }
// idx 1..N -> hide that pass; 0 -> none. Returns a short description for the menu.
std::string FxPassSelect(int idx) {
    std::lock_guard<std::mutex> l(g_fxMx);
    if (idx <= 0 || idx > (int)g_fx.size()) { g_fxHidePs = 0; return "none hidden"; }
    const FxPass& p = g_fx[idx - 1];
    g_fxHidePs = p.ps;
    char b[160];
    snprintf(b, sizeof b, "PS %08X  %ux%u fmt %d  %d tex%s", p.ps, p.w, p.h, (int)p.f, p.srvs, p.cam ? "  cam" : "");
    LOGI("d3d11: hiding effect pass %d: %s", idx, b);
    return b;
}
// VRS only into 3D scene targets: a camera draw into it in the last few frames and not the game's UI texture. The UI
// (HUD, menus, markers) is drawn into an eye-size twinned target without depth; coarse shading there made HUD text
// unreadable (user, build 23). Decided once per frame per context and bound target.
static bool VrsTarget(Ctx11* x) {
    long f = G().frame;
    if (x->vrsRtv == x->b.rtv[0] && x->vrsRtvFrame == f) return x->vrsRtvOk;
    x->vrsRtv = x->b.rtv[0]; x->vrsRtvFrame = f; x->vrsRtvOk = false;
    ID3D11Resource* r = nullptr;
    if (x->b.rtv[0]) x->b.rtv[0]->GetResource(&r);
    if (!r) return false;
    {
        std::lock_guard<std::mutex> l(g_rtUseMx);
        auto it = g_rtUse.find(r);
        x->vrsRtvOk = it != g_rtUse.end() && it->second.cam > 0 && f - it->second.lastCam <= 3 && !g_uiKnown.count(r);
    }
    r->Release();
    return x->vrsRtvOk;
}
void GpuStartMark(ID3D11DeviceContext* c);
static thread_local bool t_fxDraw = false;   // set by Hk_Draw for <= 4-vertex draws
template <class F> void StereoDraw(ID3D11DeviceContext* c, F draw) {
    struct DT { LONG64 t0 = Qpc(); ~DT() { InterlockedAdd64(&g_pfDraw, Qpc() - t0); } } dt_;
    GpuStartMark(c);
    if (!g_isoVs && !G().s.stereoIsoVs.empty()) g_isoVs = (uint32_t)strtoul(G().s.stereoIsoVs.c_str(), nullptr, 16);
    Ctx11* x = X(c);
    if (g_hideNow && x) { auto vh = g_vsHash.find(x->curVS); if (vh != g_vsHash.end() && vh->second == g_hideNow) return; }
    uint32_t psH = 0;
    if (x && (g_fxHidePs || t_fxDraw)) {
        MxGuard l; auto ph = g_psHash.find(x->curPS); if (ph != g_psHash.end()) psH = ph->second;
    }
    if (g_fxHidePs && psH == g_fxHidePs) return;
    DrawCam dc = ObserveDraw(x);
    struct DofRestore { ID3D11DeviceContext* c = nullptr; Ctx11* x = nullptr; ~DofRestore() { if (c) OC(c, PSSetConstantBuffers)(c, 1, 1, &x->psCB[1]); } } dofR_;
    if (x && g_dofPs && x->curPS == g_dofPs && !G().s.gameDof && x->psCB[1]) {
        std::vector<float> f;
        { MxGuard l; if (Shadow* sh = FindShadow(x, x->psCB[1])) if (sh->f.size() >= 44) f = sh->f; }
        if (f.size() >= 44 && f[41] != 0.f) {
            float was = f[41];
            f[41] = 0.f;
            UINT bytes = (UINT)(f.size() * 4);
            if (!x->dofCb || x->dofCbBytes != bytes) {
                if (x->dofCb) { x->dofCb->Release(); x->dofCb = nullptr; }
                ID3D11Device* dev = nullptr; c->GetDevice(&dev);
                D3D11_BUFFER_DESC bd{}; bd.ByteWidth = (bytes + 15) & ~15u; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                if (dev) { dev->CreateBuffer(&bd, nullptr, &x->dofCb); dev->Release(); }
                x->dofCbBytes = bytes;
            }
            if (x->dofCb) {
                OC(c, UpdateSubresource)(c, x->dofCb, 0, nullptr, f.data(), 0, 0);
                OC(c, PSSetConstantBuffers)(c, 1, 1, &x->dofCb);
                dofR_.c = c; dofR_.x = x;
                if (InterlockedIncrement(&g_dofPatched) == 1) LOGI("d3d11: depth of field removed (PS EEDD5866 blend %.3f -> 0)", was);
            }
        }
    }
    // VRS: only the game's draws into eye-size targets that have a right-eye twin (left pattern now, right one for the twin draw)
    bool vrsOn = false;
    if (x && vrs::g_state > 0) {
        vrsOn = G().s.vrsStrength > 0 && vrs::Ready() && x->b.rtvT[0] && !x->b.rt0IsBB && (UINT)x->vpW == g_eyeW && (UINT)x->vpH == g_eyeH &&
                (x->b.dsv || G().s.vrsFullscreen) && (!G().s.vrsSceneOnly || VrsTarget(x));
        vrs::Set(c, x, vrsOn ? 1 : 0);
        InterlockedIncrement64(&vrs::g_drawsAll); if (vrsOn) InterlockedIncrement64(&vrs::g_draws);
    }
    if (t_fxDraw && x) NoteFxPass(x, psH, dc.n > 0);
    if (x && (G().s.d3d11HudLayer || G().s.vrsStrength > 0)) NoteRtUse(x, dc.n > 0);
    if (g_traceState == 2 && x) {
        std::lock_guard<std::mutex> l(g_traceMx);
        if (g_trace.size() < 40000) {
            std::string srv;
            for (UINT i = 0; i < x->b.maxSrv && i < 8; ++i) if (x->b.srv[i]) srv += " s" + std::to_string(i) + "=" + DescRes(x->b.srv[i]) + (x->b.srvT[i] ? "*" : "");
            auto vh = g_vsHash.find(x->curVS);
            char b[900];
            std::string more;
            for (UINT i = 1; i < x->b.nrtv && i < 4; ++i) more += " rt" + std::to_string(i) + " " + DescRes(x->b.rtv[i]) + (x->b.rtvT[i] ? "*" : "");
            auto ph = g_psHash.find(x->curPS);
            snprintf(b, sizeof b, "f%ld c%d draw vs %08X ps %08X rt0 %s%s%s n%u dsv %s%s%s vp %.0fx%.0f | cam %d vp %d |%s", G().frame, TraceId(c), vh != g_vsHash.end() ? vh->second : 0,
                     ph != g_psHash.end() ? ph->second : 0, DescRes(x->b.rtv[0]).c_str(), x->b.rtvT[0] ? "*" : "", more.c_str(), x->b.nrtv, DescRes(x->b.dsv).c_str(), x->b.dsvT ? "*" : "", x->b.rt0IsBB ? " BB" : "",
                     x->vpW, x->vpH, dc.n, dc.haveVP ? 1 : 0, srv.c_str());
            g_trace.push_back(b);
            if (g_isoVs && vh != g_vsHash.end() && vh->second == g_isoVs) {   // test aid: the iso shader's camera data per draw
                for (int i = 0; i < dc.n; ++i) {
                    const SlotPatch& sp = dc.s[i];
                    Shadow* sh = FindShadow(x, sp.buf);
                    char h[400];
                    snprintf(h, sizeof h, "   iso %s cb%d buf %p (%s shadow, ver %u, %u bytes) own %d vp@%d", sp.ps ? "PS" : "VS", sp.slot, (void*)sp.buf,
                             x->deferred && x->cb.count(sp.buf) ? "context" : "global", sh ? sh->ver : 0, sh ? sh->bytes : 0, (int)sp.ownVP, sp.L.vp);
                    g_trace.push_back(h);
                }
                if (dc.haveVP) {
                    float d = 0; for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) d = std::max(d, std::fabs(dc.VP.m[i][j] - g_f.mainVP.m[i][j]));
                    char h[300];
                    snprintf(h, sizeof h, "   iso VP r3 %.3f %.3f %.3f %.3f | r0 %.4f %.4f %.4f %.4f | max diff to main VP %.4f", dc.VP.m[3][0], dc.VP.m[3][1], dc.VP.m[3][2], dc.VP.m[3][3],
                             dc.VP.m[0][0], dc.VP.m[0][1], dc.VP.m[0][2], dc.VP.m[0][3], d);
                    g_trace.push_back(h);
                }
            }
            static long dumped = 0, dumpFrame = -1, dumpedImm = 0;
            if (dumpFrame != G().frame) { dumpFrame = G().frame; dumped = 0; dumpedImm = 0; }
            if (!x->deferred && dc.n && dumpedImm++ < 24) {   // full-screen passes: every PS constant buffer
                std::lock_guard<SpinMx> l2(g_mx);
                auto ph = g_psHash.find(x->curPS);
                for (int sl = 0; sl < kCbSlots; ++sl) {
                    if (!x->psCB[sl]) continue;
                    Shadow* sh = FindShadow(x, x->psCB[sl]);
                    if (!sh) continue;
                    char h[200];
                    const ue3::CbLayout* L = nullptr;
                    if (ph != g_psHash.end()) { auto it = g_psInfo.find(ph->second); if (it != g_psInfo.end()) L = &it->second.slot[sl]; }
                    snprintf(h, sizeof h, "   PS %08X cb%d bytes %u layout vp %d v %d p %d cam %d s2w %d ivp %d iv %d prev vp %d v %d p %d", ph != g_psHash.end() ? ph->second : 0, sl, sh->bytes,
                             L ? L->vp : -9, L ? L->v : -9, L ? L->p : -9, L ? L->cam : -9, L ? L->s2w : -9, L ? L->ivp : -9, L ? L->iv : -9, L ? L->vp2 : -9, L ? L->v2 : -9, L ? L->p2 : -9);
                    g_trace.push_back(h);
                    int n = (int)(sh->f.size() / 4);
                    for (int r = 0; r < n && r < 104; ++r) {
                        snprintf(h, sizeof h, "     r%-2d %14.6f %14.6f %14.6f %14.6f", r, sh->f[r * 4], sh->f[r * 4 + 1], sh->f[r * 4 + 2], sh->f[r * 4 + 3]);
                        g_trace.push_back(h);
                    }
                }
            }
            if (dc.n && dumped++ < 6) {
                for (int i = 0; i < dc.n; ++i) {
                    const SlotPatch& sp = dc.s[i];
                    std::lock_guard<SpinMx> l2(g_mx);
                    Shadow* sh = FindShadow(x, sp.buf);
                    if (!sh) continue;
                    char h[200];
                    snprintf(h, sizeof h, "   %s cb%d vp@%d%s cam@%d s2w@%d ivp@%d own %d bytes %u", sp.ps ? "PS" : "VS", sp.slot, sp.L.vp, sp.L.vpT ? "T" : "", sp.L.cam,
                             sp.L.s2w, sp.L.ivp, (int)sp.ownVP, sh->bytes);
                    g_trace.push_back(h);
                    int n = (int)(sh->f.size() / 4);
                    for (int r = 0; r < n && r < 40; ++r) {
                        snprintf(h, sizeof h, "     r%-2d %12.5f %12.5f %12.5f %12.5f", r, sh->f[r * 4], sh->f[r * 4 + 1], sh->f[r * 4 + 2], sh->f[r * 4 + 3]);
                        g_trace.push_back(h);
                    }
                }
                EyeMats11 em;
                if (dc.haveVP && StereoEyeMats11(&dc.VP.m[0][0], &em))
                    for (int e = 0; e < 2; ++e)
                        for (int r = 0; r < 4; ++r) {
                            char h[160];
                            snprintf(h, sizeof h, "     eye%d VP r%d %12.5f %12.5f %12.5f %12.5f", e, r, em.VPe[e][r * 4], em.VPe[e][r * 4 + 1], em.VPe[e][r * 4 + 2], em.VPe[e][r * 4 + 3]);
                            g_trace.push_back(h);
                        }
            }
        }
    }
    if (!x || !g_f.active) { draw(); return; }
    Bind11& g_b = x->b;
    if (g_b.rt0IsBB) {
        if (!g_f.composited && g_b.nTwinSrv > 0 && CompositeStarts(c, x)) { draw(); DrawIntoEyes(c, x, draw, true); return; }
        draw();
        if (g_f.composited) {   // UI over the scene: the HUD layer (or, d3d11_hud_layer = 0, into both eyes at screen depth)
            if (G().s.d3d11HudLayer) DrawIntoHud(c, x, draw);
            else DrawIntoEyes(c, x, draw, false);
            g_f.hud++;
        }
        return;
    }
    if (!g_b.rtvT[0] && !g_b.dsvT) {   // eye-independent target (shadow maps)
        if (g_b.nTwinSrv > 0 && g_b.rtv[0]) {   // ...unless it reads eye images (would need a twin)
            g_f.sharedOut++;
            static int logged = 0;
            if (logged < 8) {
                ID3D11Resource* r = nullptr;
                g_b.rtv[0]->GetResource(&r);
                D3D11_TEXTURE2D_DESC td{};
                if (r) { ((ID3D11Texture2D*)r)->GetDesc(&td); r->Release(); }
                static UINT seen[8][3];
                bool dup = false;
                for (int i = 0; i < logged; ++i) dup |= seen[i][0] == td.Width && seen[i][1] == td.Height && seen[i][2] == (UINT)td.Format;
                if (!dup) {
                    seen[logged][0] = td.Width; seen[logged][1] = td.Height; seen[logged][2] = td.Format; logged++;
                    LOGI("d3d11 stereo: draw into a shared target %ux%u format %d (array %u, bind %x) reads eye textures", td.Width, td.Height,
                         (int)td.Format, td.ArraySize, td.BindFlags);
                }
            }
        }
        draw();
        return;
    }
    if (!g_warnedSharedDepth && g_b.rtvT[0] && g_b.dsv && !g_b.dsvT) {
        g_warnedSharedDepth = true;
        LOGW("d3d11 stereo: a stereo draw uses a depth buffer without a twin - the right eye may be wrong");
    }
    if (g_isoVs && x) {   // test aid stereo_iso_vs: 1 = no camera patch for this VS, 2 = left eye only
        auto vh = g_vsHash.find(x->curVS);
        if (vh != g_vsHash.end() && vh->second == g_isoVs) {
            if (G().s.stereoIsoMode == 1) { draw(); BindEye(c, x, true); draw(); BindEye(c, x, false); return; }
            if (G().s.stereoIsoMode == 2) { int q = g_f.patch && dc.n ? PatchBind(c, x, 0, dc) : 0; draw(); if (q) RestoreCBs(c, x, dc); return; }
        }
    }
    int p = g_f.patch && dc.n ? PatchBind(c, x, 0, dc) : 0;
    { AddTimer t(&g_pfGameL); draw(); }
    if (G().s.testLeftOnly) { if (p) RestoreCBs(c, x, dc); return; }   // perf test: no right eye
    { AddTimer t(&g_pfBind); BindEye(c, x, true); }
    if (vrsOn) vrs::Set(c, x, 2);
    if (g_f.patch && dc.n) p += PatchBind(c, x, 1, dc);
    { AddTimer t(&g_pfGameR); draw(); }
    AddTimer tb(&g_pfBind);
    if (vrsOn) vrs::Set(c, x, 1);
    BindEye(c, x, false);
    if (p) { RestoreCBs(c, x, dc); g_f.patched++; if (x->deferred) { if (!x->recPatched) x->recFrame = G().frame; x->recPatched++; } }
    g_f.right++;
}
void STDMETHODCALLTYPE Hk_Draw(ID3D11DeviceContext* c, UINT a, UINT b) {
    if (g_bypass11) return OC(c, Draw)(c, a, b);
    t_fxDraw = a <= 4; StereoDraw(c, [&] { OC(c, Draw)(c, a, b); }); t_fxDraw = false; }
void STDMETHODCALLTYPE Hk_DrawIndexed(ID3D11DeviceContext* c, UINT a, UINT b, INT d) {
    if (g_bypass11) return OC(c, DrawIndexed)(c, a, b, d); StereoDraw(c, [&] { OC(c, DrawIndexed)(c, a, b, d); }); }
void STDMETHODCALLTYPE Hk_DrawInstanced(ID3D11DeviceContext* c, UINT a, UINT b, UINT d, UINT e) {
    if (g_bypass11) return OC(c, DrawInstanced)(c, a, b, d, e);
    StereoDraw(c, [&] { OC(c, DrawInstanced)(c, a, b, d, e); });
}
void STDMETHODCALLTYPE Hk_DrawIndexedInstanced(ID3D11DeviceContext* c, UINT a, UINT b, UINT d, INT e, UINT f) {
    if (g_bypass11) return OC(c, DrawIndexedInstanced)(c, a, b, d, e, f);
    StereoDraw(c, [&] { OC(c, DrawIndexedInstanced)(c, a, b, d, e, f); });
}

// ---- compute: a dispatch writing a twinned texture (UAV) runs again for the right eye with twin UAVs / SRVs
HRESULT STDMETHODCALLTYPE Hk_CreateUnorderedAccessView(ID3D11Device* dev, ID3D11Resource* r, const D3D11_UNORDERED_ACCESS_VIEW_DESC* d,
                                                       ID3D11UnorderedAccessView** out) {
    if (g_bypass11) return o_D11CreateUnorderedAccessView(dev, r, d, out);
    HRESULT hr = o_D11CreateUnorderedAccessView(dev, r, d, out);
    if (SUCCEEDED(hr) && out && *out)
        if (ID3D11Resource* t = Twin11(r)) {
            ID3D11UnorderedAccessView* v = nullptr;
            if (SUCCEEDED(o_D11CreateUnorderedAccessView(dev, t, d, &v)) && v) { Attach11(*out, v); v->Release(); }
        }
    return hr;
}
void STDMETHODCALLTYPE Hk_CSSetShaderResources(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11ShaderResourceView* const* v) {
    if (g_bypass11) return OC(c, CSSetShaderResources)(c, s, n, v);
    if (Ctx11* x = X(c)) for (UINT i = 0; i < n && s + i < kCs; ++i) { x->csSrv[s + i] = v ? v[i] : nullptr; x->csSrvT[s + i] = Twin11(x->csSrv[s + i]); }
    OC(c, CSSetShaderResources)(c, s, n, v);
}
void STDMETHODCALLTYPE Hk_CSSetUnorderedAccessViews(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11UnorderedAccessView* const* v, const UINT* cnt) {
    if (g_bypass11) return OC(c, CSSetUnorderedAccessViews)(c, s, n, v, cnt);
    if (Ctx11* x = X(c)) for (UINT i = 0; i < n && s + i < kUav; ++i) { x->csUav[s + i] = v ? v[i] : nullptr; x->csUavT[s + i] = Twin11(x->csUav[s + i]); }
    OC(c, CSSetUnorderedAccessViews)(c, s, n, v, cnt);
}
template <class F> void StereoDispatch(ID3D11DeviceContext* c, F run) {
    run();
    Ctx11* x = X(c);
    if (!x) return;
    if (g_traceState == 2) {
        std::lock_guard<std::mutex> l(g_traceMx);
        std::string u;
        for (UINT i = 0; i < kUav; ++i) if (x->csUav[i]) u += " u" + std::to_string(i) + "=" + DescRes(x->csUav[i]) + (x->csUavT[i] ? "*" : "");
        for (UINT i = 0; i < 6; ++i) if (x->csSrv[i]) u += " s" + std::to_string(i) + "=" + DescRes(x->csSrv[i]) + (x->csSrvT[i] ? "*" : "");
        g_trace.push_back("dispatch c" + std::to_string(TraceId(c)) + u);
    }
    ID3D11ShaderResourceView** g_csSrv = x->csSrv; ID3D11ShaderResourceView** g_csSrvT = x->csSrvT;
    ID3D11UnorderedAccessView** g_csUav = x->csUav; ID3D11UnorderedAccessView** g_csUavT = x->csUavT;
    g_f.dispatch++;
    if (!g_f.active) return;
    bool twinOut = false;
    for (UINT i = 0; i < kUav; ++i) twinOut |= g_csUavT[i] != nullptr;
    if (!twinOut) return;
    ID3D11UnorderedAccessView* u[kUav];
    ID3D11ShaderResourceView* s[kCs];
    for (UINT i = 0; i < kUav; ++i) u[i] = g_csUavT[i] ? g_csUavT[i] : g_csUav[i];
    for (UINT i = 0; i < kCs; ++i) s[i] = g_csSrvT[i] ? g_csSrvT[i] : g_csSrv[i];
    ID3D11UnorderedAccessView* none[kUav] = {};
    OC(c, CSSetUnorderedAccessViews)(c, 0, kUav, none, nullptr);   // no resource bound as UAV and SRV at once
    OC(c, CSSetShaderResources)(c, 0, kCs, s);
    OC(c, CSSetUnorderedAccessViews)(c, 0, kUav, u, nullptr);
    run();
    OC(c, CSSetUnorderedAccessViews)(c, 0, kUav, none, nullptr);
    OC(c, CSSetShaderResources)(c, 0, kCs, g_csSrv);
    OC(c, CSSetUnorderedAccessViews)(c, 0, kUav, g_csUav, nullptr);   // (append counters are not restored; UE3 uses none here)
    g_f.dispatchMirrored++;
}
void STDMETHODCALLTYPE Hk_Dispatch(ID3D11DeviceContext* c, UINT x, UINT y, UINT z) {
    if (g_bypass11) return OC(c, Dispatch)(c, x, y, z); StereoDispatch(c, [&] { OC(c, Dispatch)(c, x, y, z); }); }
void STDMETHODCALLTYPE Hk_DispatchIndirect(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT o) {
    if (g_bypass11) return OC(c, DispatchIndirect)(c, b, o); StereoDispatch(c, [&] { OC(c, DispatchIndirect)(c, b, o); }); }
void STDMETHODCALLTYPE Hk_DrawAuto(ID3D11DeviceContext* c) {
    if (g_bypass11) return OC(c, DrawAuto)(c); if (X(c)) g_f.indirect++; OC(c, DrawAuto)(c); }
void STDMETHODCALLTYPE Hk_DrawInstancedIndirect(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT o) {
    if (g_bypass11) return OC(c, DrawInstancedIndirect)(c, b, o); if (X(c)) g_f.indirect++; OC(c, DrawInstancedIndirect)(c, b, o); }
void STDMETHODCALLTYPE Hk_DrawIndexedInstancedIndirect(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT o) {
    if (g_bypass11) return OC(c, DrawIndexedInstancedIndirect)(c, b, o);
    if (X(c)) g_f.indirect++;
    OC(c, DrawIndexedInstancedIndirect)(c, b, o);
}
void STDMETHODCALLTYPE Hk_VSSetShaderResources(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11ShaderResourceView* const* v) {
    if (g_bypass11) return OC(c, VSSetShaderResources)(c, s, n, v);
    if (X(c) && v) for (UINT i = 0; i < n; ++i) if (Twin11(v[i])) { g_f.vsTwinSrv++; break; }
    OC(c, VSSetShaderResources)(c, s, n, v);
}

// ---- mirrored clears / copies
void STDMETHODCALLTYPE Hk_ClearRenderTargetView(ID3D11DeviceContext* c, ID3D11RenderTargetView* v, const FLOAT* col) {
    if (g_bypass11) return OC(c, ClearRenderTargetView)(c, v, col);
    OC(c, ClearRenderTargetView)(c, v, col);
    if (G().s.d3d11HudLayer && Twin11(v)) NoteClear(v, col);
    if (g_traceState == 2) { std::lock_guard<std::mutex> l(g_traceMx); g_trace.push_back("clearRT c" + std::to_string(TraceId(c)) + " " + DescRes(v) + (Twin11(v) ? "*" : "")); }
    if (X(c)) if (auto t = Twin11(v)) { OC(c, ClearRenderTargetView)(c, t, col); g_f.mirrored++; }
}
void STDMETHODCALLTYPE Hk_ClearDepthStencilView(ID3D11DeviceContext* c, ID3D11DepthStencilView* v, UINT fl, FLOAT z, UINT8 s) {
    if (g_bypass11) return OC(c, ClearDepthStencilView)(c, v, fl, z, s);
    OC(c, ClearDepthStencilView)(c, v, fl, z, s);
    if (X(c)) if (auto t = Twin11(v)) { OC(c, ClearDepthStencilView)(c, t, fl, z, s); g_f.mirrored++; }
}
// a copy of the scene into the back buffer is a composite too
bool CopyComposite(ID3D11DeviceContext* c, Ctx11* x, ID3D11Resource* dst, ID3D11Resource* src) {
    if (dst != g_bbRes || !Twin11(src)) return false;
    D3D11_TEXTURE2D_DESC sd{};
    ((ID3D11Texture2D*)src)->GetDesc(&sd);
    if (sd.Width != g_eyeW || sd.Height != g_eyeH || sd.SampleDesc.Count != 1 || Typeless(sd.Format) != g_eyeFmt) return false;
    if (!CompositeStarts(c, x)) return false;
    OC(c, CopyResource)(c, g_eyeTex[0], src);
    OC(c, CopyResource)(c, g_eyeTex[1], Twin11(src));
    return true;
}
// GreedFall's occlusion culling: a 128x72 R32 level of the depth pyramid of the eye image is copied to a CPU-readable
// texture every frame; the engine tests objects against it with its own camera. In VR that depth was rendered by the
// left eye with the headset's projection, which does not line up with the engine's (wider, symmetric) projection, so
// objects in the background next to near ones were judged hidden (user: "something small vanishes in the background
// when turning the head"). occlusion_mode 1: the copy is re-mapped to the game's projection (per axis: game NDC -> view
// tangent -> eye NDC; reversed-Z infinite depth with the same near plane needs no depth change), conservatively (the
// farthest of 4 taps), and parts the eye did not see get far depth. 2: far depth everywhere (no occlusion culling;
// test 080: 60 -> 32 fps).
extern ID3D11VertexShader* g_fsVS; bool EnsureFsShaders();
static ID3D11Texture2D* g_occZero = nullptr; static UINT g_occW = 0, g_occH = 0; static DXGI_FORMAT g_occF = DXGI_FORMAT_UNKNOWN;
static ID3D11Texture2D* g_occTmp = nullptr; static ID3D11RenderTargetView* g_occRtv = nullptr; static ID3D11PixelShader* g_occPS = nullptr;
static ID3D11Buffer* g_occCB = nullptr; static ID3D11Resource* g_occSrcRes = nullptr; static ID3D11ShaderResourceView* g_occSrv = nullptr;
static bool g_occFailed = false;
static bool EnsureOccRemap(ID3D11Resource* src, const D3D11_TEXTURE2D_DESC& sd) {
    if (g_occFailed || !EnsureFsShaders()) return false;
    Bypass11 bp;
    if (!g_occPS) {
        typedef HRESULT(WINAPI* Compile_t)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
        HMODULE m = LoadLibraryA("d3dcompiler_47.dll");
        Compile_t comp = m ? (Compile_t)GetProcAddress(m, "D3DCompile") : nullptr;
        const char* ps =
            "Texture2D<float> t : register(t0); cbuffer C : register(b0) { float4 k; float4 b; float4 r; float4 h; };"
            "float main(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target {"
            "  float2 ng = float2(uv.x * 2 - 1, 1 - uv.y * 2);"
            "  float2 ne = ng / k.xy * k.zw + b.xy;"
            "  float2 ue = float2(ne.x * 0.5 + 0.5, 0.5 - ne.y * 0.5);"
            "  if (any(ue < 0) || any(ue > 1)) {"
            "    float2 nr = ng / k.xy * r.xy + r.zw;"   // right eye NDC
            "    bool seen = all(abs(ne) <= h.x) || all(abs(nr) <= h.x);"
            "    return (h.y > 0 && !seen) ? 1 : 0; }"
            "  float2 px = ue * b.zw - 0.5; int2 i0 = int2(floor(px)); int2 mx = int2(b.zw) - 1;"
            "  float d = 1e30;"
            "  [unroll] for (int y = 0; y < 2; ++y) [unroll] for (int x = 0; x < 2; ++x) d = min(d, t.Load(int3(clamp(i0 + int2(x, y), 0, mx), 0)));"
            "  return d; }";
        ID3DBlob *bpx = nullptr, *err = nullptr;
        if (!comp || FAILED(comp(ps, strlen(ps), nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, &bpx, &err)) ||
            FAILED(g_dev->CreatePixelShader(bpx->GetBufferPointer(), bpx->GetBufferSize(), nullptr, &g_occPS))) {
            LOGW("d3d11: occlusion re-map shader failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
            if (err) err->Release(); if (bpx) bpx->Release(); g_occFailed = true; return false;
        }
        bpx->Release();
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 64; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(g_dev->CreateBuffer(&bd, nullptr, &g_occCB))) { g_occFailed = true; return false; }
    }
    D3D11_TEXTURE2D_DESC td{};
    if (g_occTmp) g_occTmp->GetDesc(&td);
    if (!g_occTmp || td.Width != sd.Width || td.Height != sd.Height) {
        if (g_occRtv) g_occRtv->Release(); if (g_occTmp) g_occTmp->Release(); g_occRtv = nullptr; g_occTmp = nullptr;
        D3D11_TEXTURE2D_DESC d{}; d.Width = sd.Width; d.Height = sd.Height; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R32_FLOAT;
        d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_occTmp)) || FAILED(g_dev->CreateRenderTargetView(g_occTmp, nullptr, &g_occRtv))) { g_occFailed = true; return false; }
    }
    if (g_occSrcRes != src) {
        if (g_occSrv) g_occSrv->Release(); g_occSrv = nullptr; g_occSrcRes = nullptr;
        if (!(sd.BindFlags & D3D11_BIND_SHADER_RESOURCE)) { g_occFailed = true; LOGW("d3d11: occlusion depth has no shader view - not re-mapped"); return false; }
        D3D11_SHADER_RESOURCE_VIEW_DESC v{}; v.Format = DXGI_FORMAT_R32_FLOAT; v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; v.Texture2D.MipLevels = 1;
        if (FAILED(g_dev->CreateShaderResourceView(src, &v, &g_occSrv))) return false;
        g_occSrcRes = src;
    }
    return true;
}
bool OcclusionReadback(ID3D11DeviceContext* c, ID3D11Resource* dst, ID3D11Resource* src) {
    D3D11_RESOURCE_DIMENSION dim;
    src->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return false;
    D3D11_TEXTURE2D_DESC sd{}, dd{};
    ((ID3D11Texture2D*)src)->GetDesc(&sd);
    if (sd.Width > 512 || sd.ArraySize != 1 || sd.MipLevels != 1 || (sd.Format != DXGI_FORMAT_R32_FLOAT && sd.Format != DXGI_FORMAT_R32_TYPELESS)) return false;
    dst->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return false;
    ((ID3D11Texture2D*)dst)->GetDesc(&dd);
    if (!(dd.CPUAccessFlags & D3D11_CPU_ACCESS_READ) || dd.Width != sd.Width || dd.Height != sd.Height) return false;
    int mode = G().s.gfOcclusion;
    static int logged = 0;
    if (!logged++) LOGI("d3d11: occlusion readback found: %ux%u format %d (eye depth -> CPU), occlusion_mode %d", sd.Width, sd.Height, (int)sd.Format, mode);
    if (mode == 1 || mode == 3) {
        if (!g_f.mainEmOk || !EnsureOccRemap(src, sd)) return false;
        const EyeMats11& em = g_f.mainEm;
        const float* Pe = em.Pe[0]; const float* Pr = em.Pe[1];
        float cb[16] = {em.P00, em.P11, Pe[0] / Pe[11], Pe[5] / Pe[11], Pe[8] / Pe[11], Pe[9] / Pe[11], (float)sd.Width, (float)sd.Height,
                        Pr[0] / Pr[11], Pr[5] / Pr[11], Pr[8] / Pr[11], Pr[9] / Pr[11], G().s.occMargin, mode == 3 ? 1.f : 0.f, 0, 0};
        static int nlog = 0;
        if (nlog++ < 2) LOGI("d3d11: occlusion re-map: game tan %.3f x %.3f, left eye scale %.3f x %.3f offset %.3f %.3f", 1 / cb[0], 1 / cb[1], cb[2], cb[3], cb[4], cb[5]);
        VrsOffForMod(c);
        Bypass11 bp;
        ID3D11VertexShader* vs = nullptr; ID3D11PixelShader* ps = nullptr; ID3D11InputLayout* il = nullptr; D3D11_PRIMITIVE_TOPOLOGY topo;
        ID3D11BlendState* bs = nullptr; FLOAT bf[4]; UINT mask = 0; ID3D11DepthStencilState* ds = nullptr; UINT sref = 0; ID3D11RasterizerState* rs = nullptr;
        UINT nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE; D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
        ID3D11ShaderResourceView* srv0 = nullptr; ID3D11Buffer* cb0 = nullptr;
        ID3D11RenderTargetView* rtv[8] = {}; ID3D11DepthStencilView* dsv = nullptr;
        c->OMGetRenderTargets(8, rtv, &dsv);
        c->VSGetShader(&vs, nullptr, nullptr); c->PSGetShader(&ps, nullptr, nullptr); c->IAGetInputLayout(&il); c->IAGetPrimitiveTopology(&topo);
        c->OMGetBlendState(&bs, bf, &mask); c->OMGetDepthStencilState(&ds, &sref); c->RSGetState(&rs); c->RSGetViewports(&nvp, vps);
        c->PSGetShaderResources(0, 1, &srv0); c->PSGetConstantBuffers(0, 1, &cb0);
        c->UpdateSubresource(g_occCB, 0, nullptr, cb, 0, 0);
        D3D11_VIEWPORT v{0, 0, (float)sd.Width, (float)sd.Height, 0, 1};
        c->OMSetRenderTargets(1, &g_occRtv, nullptr);
        c->VSSetShader(g_fsVS, nullptr, 0); c->PSSetShader(g_occPS, nullptr, 0); c->IASetInputLayout(nullptr);
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); c->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        c->OMSetDepthStencilState(nullptr, 0); c->RSSetState(nullptr); c->RSSetViewports(1, &v);
        c->PSSetShaderResources(0, 1, &g_occSrv); c->PSSetConstantBuffers(0, 1, &g_occCB);
        c->Draw(3, 0);
        ID3D11ShaderResourceView* nul = nullptr; c->PSSetShaderResources(0, 1, &nul);
        c->CopyResource(dst, g_occTmp);
        c->OMSetRenderTargets(8, rtv, dsv);
        c->VSSetShader(vs, nullptr, 0); c->PSSetShader(ps, nullptr, 0); c->IASetInputLayout(il); c->IASetPrimitiveTopology(topo);
        c->OMSetBlendState(bs, bf, mask); c->OMSetDepthStencilState(ds, sref); c->RSSetState(rs); c->RSSetViewports(nvp, vps);
        c->PSSetShaderResources(0, 1, &srv0); c->PSSetConstantBuffers(0, 1, &cb0);
        for (auto* r : rtv) if (r) r->Release(); if (dsv) dsv->Release();
        if (vs) vs->Release(); if (ps) ps->Release(); if (il) il->Release(); if (bs) bs->Release(); if (ds) ds->Release(); if (rs) rs->Release();
        if (srv0) srv0->Release(); if (cb0) cb0->Release();
        return true;
    }
    if (mode != 2) return false;
    if (!g_occZero || g_occW != dd.Width || g_occH != dd.Height || g_occF != dd.Format) {
        if (g_occZero) { g_occZero->Release(); g_occZero = nullptr; }
        D3D11_TEXTURE2D_DESC zd = dd;
        zd.Usage = D3D11_USAGE_DEFAULT; zd.CPUAccessFlags = 0; zd.BindFlags = 0; zd.MiscFlags = 0; zd.MipLevels = 1;
        std::vector<uint32_t> zeros((size_t)dd.Width * dd.Height, 0);
        D3D11_SUBRESOURCE_DATA init{zeros.data(), dd.Width * 4, 0};
        Bypass11 bp;
        if (FAILED(g_dev->CreateTexture2D(&zd, &init, &g_occZero))) { g_occZero = nullptr; return false; }
        g_occW = dd.Width; g_occH = dd.Height; g_occF = dd.Format;
    }
    OC(c, CopyResource)(c, dst, g_occZero);
    return true;
}
void STDMETHODCALLTYPE Hk_CopyResource(ID3D11DeviceContext* c, ID3D11Resource* dst, ID3D11Resource* src) {
    if (g_bypass11) return OC(c, CopyResource)(c, dst, src);
    OC(c, CopyResource)(c, dst, src);
    if (g_traceState == 2) { std::lock_guard<std::mutex> l(g_traceMx); g_trace.push_back("copy c" + std::to_string(TraceId(c)) + " " + DescR(dst) + (Twin11(dst) ? "*" : "") + (dst == g_bbRes ? " BB" : "") + " <- " + DescR(src) + (Twin11(src) ? "*" : "")); }
    Ctx11* x = X(c);
    if (!x) return;
    if (auto t = Twin11(dst)) { auto s = Twin11(src); OC(c, CopyResource)(c, t, s ? s : src); g_f.mirrored++; }
    else if (Twin11(src) && OcclusionReadback(c, dst, src)) {}
    else if (g_f.active) CopyComposite(c, x, dst, src);
}
void STDMETHODCALLTYPE Hk_CopySubresourceRegion(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT ds, UINT x, UINT y, UINT z, ID3D11Resource* src,
                                                UINT ss, const D3D11_BOX* box) {
    if (g_bypass11) return OC(c, CopySubresourceRegion)(c, dst, ds, x, y, z, src, ss, box);
    OC(c, CopySubresourceRegion)(c, dst, ds, x, y, z, src, ss, box);
    if (g_traceState == 2) { std::lock_guard<std::mutex> l(g_traceMx); g_trace.push_back("copyRegion c" + std::to_string(TraceId(c)) + " " + DescR(dst) + (Twin11(dst) ? "*" : "") + (dst == g_bbRes ? " BB" : "") + " <- " + DescR(src) + (Twin11(src) ? "*" : "") + (box ? " box" : "")); }
    Ctx11* cx = X(c);
    if (!cx) return;
    if (auto t = Twin11(dst)) { auto s = Twin11(src); OC(c, CopySubresourceRegion)(c, t, ds, x, y, z, s ? s : src, ss, box); g_f.mirrored++; }
    else if (g_f.active && !box && !x && !y && !ds && !ss) CopyComposite(c, cx, dst, src);
}
void STDMETHODCALLTYPE Hk_ResolveSubresource(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT ds, ID3D11Resource* src, UINT ss, DXGI_FORMAT f) {
    if (g_bypass11) return OC(c, ResolveSubresource)(c, dst, ds, src, ss, f);
    OC(c, ResolveSubresource)(c, dst, ds, src, ss, f);
    if (g_traceState == 2) { std::lock_guard<std::mutex> l(g_traceMx); g_trace.push_back("resolve c" + std::to_string(TraceId(c)) + " " + DescR(dst) + (Twin11(dst) ? "*" : "") + " <- " + DescR(src) + (Twin11(src) ? "*" : "")); }
    if (!X(c)) return;
    if (auto t = Twin11(dst)) { auto s = Twin11(src); OC(c, ResolveSubresource)(c, t, ds, s ? s : src, ss, f); g_f.mirrored++; }
}

// ---- deferred contexts / command lists
struct CtxSlot { int slot; void* hook; void** orig; void* target; };   // target: immediate-context function we detoured inline
std::vector<CtxSlot> g_ctxSlots;   // every context slot we hook (filled in HookDevice)
void HookDeferredVtable(ID3D11DeviceContext* dc) {
    void** vt = *(void***)dc;
    if ((void*)vt == g_immVtbl) return;   // shares the immediate context's vtable: already hooked
    std::lock_guard<std::mutex> l(g_defVtMx);
    for (long i = 0; i < g_nDefVt; ++i) if (g_defVt[i].vt == vt) return;
    if (g_nDefVt >= 64) { LOGW("d3d11: more than 64 deferred-context vtables - context %p not hooked", (void*)dc); return; }
    DefVt& d = g_defVt[g_nDefVt];
    d.vt = vt;
    int hooked = 0, shared = 0;
    std::string sharedNames;
    for (auto& cs : g_ctxSlots) {
        if (vt[cs.slot] == cs.hook) continue;
        // Same driver function as the immediate context: it is already detoured inline into our hook. Recording it as
        // this vtable's original would make the hook call itself forever (stack overflow on the user's RTX 5090 driver:
        // FinishCommandList). Leave the slot alone; OC() then uses the immediate trampoline, which is the right function.
        if (vt[cs.slot] == cs.target) { shared++; if (sharedNames.size() < 400) sharedNames += std::to_string(cs.slot) + " "; continue; }
        d.orig[cs.slot] = vt[cs.slot];
        HookVtableRaw(vt, cs.slot, cs.hook);
        hooked++;
    }
    MemoryBarrier();
    InterlockedIncrement(&g_nDefVt);
    static int logged = 0;
    if (logged++ < 4 || shared) LOGI("d3d11: deferred-context vtable %p: %d slots hooked, %d shared with the immediate context (already detoured) %s",
                                     (void*)vt, hooked, shared, sharedNames.c_str());
}
HRESULT STDMETHODCALLTYPE Hk_CreateDeferredContext(ID3D11Device* d, UINT fl, ID3D11DeviceContext** out) {
    HRESULT hr = o_D11CreateDeferredContext(d, fl, out);
    if (g_bypass11 || FAILED(hr) || !out || !*out) return hr;
    HookDeferredVtable(*out);
    RegisterDeferred(*out);
    if (g_deferredCtx <= 16) LOGI("d3d11: deferred context %p created (%u so far)", (void*)*out, g_deferredCtx);
    return hr;
}
uint64_t g_vpSmall = 0, g_vpFull = 0;   // camera draws into a target with a smaller / full viewport (dynamic resolution)
void STDMETHODCALLTYPE Hk_RSSetViewports(ID3D11DeviceContext* c, UINT n, const D3D11_VIEWPORT* v) {
    if (!g_bypass11 && n && v) if (Ctx11* x = X(c)) { x->vpW = v[0].Width; x->vpH = v[0].Height; }
    OC(c, RSSetViewports)(c, n, v);
}
// Command-list age: a list replayed in a later frame than its recording carries the head pose of that older frame in
// its per-eye camera buffers (diagnostics for "the ground moves with the HMD").
struct ListInfo { long frame; uint32_t patched; uint32_t execs; };
std::unordered_map<ID3D11CommandList*, ListInfo> g_lists;
std::mutex g_listMx;
struct ListStats { uint64_t execs = 0, fresh = 0, older1 = 0, older2 = 0, patchedFresh = 0, patchedStale = 0, reExec = 0, unknown = 0; } g_ls;
std::string ListStatsLine() {
    char b[300];
    snprintf(b, sizeof b, "%llu list executions: %llu recorded this frame, %llu one frame old, %llu older, %llu re-executed, %llu unknown; camera-patched draws in "
             "fresh lists %llu, in old lists %llu", g_ls.execs, g_ls.fresh, g_ls.older1, g_ls.older2, g_ls.reExec, g_ls.unknown, g_ls.patchedFresh, g_ls.patchedStale);
    g_ls = ListStats();
    return b;
}
HRESULT STDMETHODCALLTYPE Hk_FinishCommandList(ID3D11DeviceContext* c, BOOL restore, ID3D11CommandList** out) {
    HRESULT hr = OC(c, FinishCommandList)(c, restore, out);
    if (!g_bypass11) if (Ctx11* x = X(c)) x->vrsState = -1;   // a deferred context starts the next list with default state
    if (!g_bypass11 && SUCCEEDED(hr) && out && *out) if (Ctx11* x = X(c)) {
        std::lock_guard<std::mutex> l(g_listMx);
        g_lists[*out] = ListInfo{x->recPatched ? x->recFrame : G().frame, x->recPatched, 0};
        x->recPatched = 0; x->recFrame = -1;
        if (g_lists.size() > 4096) g_lists.clear();
    }
    if (g_traceState == 2 && out) { std::lock_guard<std::mutex> l(g_traceMx); g_trace.push_back("finish c" + std::to_string(TraceId(c)) + " -> list L" + std::to_string(TraceId(*out))); }
    // Per-eye camera buffers belong to the context, but the game executes its command lists in any order: a list must
    // not rely on an upload recorded in an earlier list of the same context (another list may have overwritten the
    // buffer in between -> an object drawn with another object's data for one frame: stretched geometry / streaks
    // flashing up in the sky). Every new list uploads again on first use.
    if (!g_bypass11 && SUCCEEDED(hr) && G().s.listCbReupload) if (Ctx11* x = X(c)) for (auto& kv : x->patch) { kv.second.frame[0] = kv.second.frame[1] = -1; }
    if (!g_bypass11 && !restore) if (Ctx11* x = X(c)) x->ResetBindings();   // a deferred context starts over with default state
    return hr;
}
void STDMETHODCALLTYPE Hk_ExecuteCommandList(ID3D11DeviceContext* c, ID3D11CommandList* cl, BOOL restore) {
    if (g_traceState == 2) { std::lock_guard<std::mutex> l(g_traceMx); g_trace.push_back("execute c" + std::to_string(TraceId(c)) + " list L" + std::to_string(TraceId(cl))); }
    if (!g_bypass11) GpuStartMark(c);
    OC(c, ExecuteCommandList)(c, cl, restore);
    if (Ctx11* x = X(c)) x->vrsState = -1;   // the list's state (VRS included) is left on / reset on the immediate context
    if (g_bypass11) return;
    g_st.cmdLists++;
    {
        std::lock_guard<std::mutex> l(g_listMx);
        g_ls.execs++;
        auto it = g_lists.find(cl);
        if (it == g_lists.end()) g_ls.unknown++;
        else {
            ListInfo& li = it->second;
            long age = G().frame - li.frame;
            if (li.execs++) g_ls.reExec++;
            if (age <= 0) { g_ls.fresh++; g_ls.patchedFresh += li.patched; }
            else { (age == 1 ? g_ls.older1 : g_ls.older2)++; g_ls.patchedStale += li.patched; }
            static int nlog = 0;
            if (age > 0 && li.patched && nlog < 12) { nlog++; LOGI("d3d11: list %p with %u camera-patched draws executed %ld frame(s) after recording (execution #%u)", (void*)cl, li.patched, age, li.execs); }
        }
    }
    if (!restore) if (Ctx11* x = X(c)) x->ResetBindings();   // the immediate context's state is cleared after the list
}

// After Present: close this frame, set up the next.
void FlushTrace() {
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%strace11_f%06ld.txt", G().runDir.c_str(), G().frame);
    if (FILE* f = _wfopen(path, L"w")) {
        std::lock_guard<std::mutex> l(g_traceMx);
        for (auto& t : g_trace) { fputs(t.c_str(), f); fputc('\n', f); }
        fclose(f);
        LOGI("d3d11: frame trace written: %S (%u lines)", path, (unsigned)g_trace.size());
    }
    g_trace.clear();
}
void EndFrame11() {
    HideCycleTick();
    { static bool capDone = false; if (!capDone && G().s.captureAtSec > 0 && SecondsSinceStart() >= G().s.captureAtSec) { capDone = true; g_captureReq = true; } }   // same as F11 (trace + atlases)
    {   // capture key (F11): 3-frame trace + headset images of 6 frames (2 frames apart) into GFVR\run_*
        static bool down = false;
        bool d = G().s.captureKey && (GetAsyncKeyState(G().s.captureKey) & 0x8000);
        if ((d && !down || g_captureReq) && (g_traceState == 0 || g_traceState == 3)) {
            g_traceState = 1; g_traceFrames = 0; g_snapLeft = 6; g_snapNext = 0;
            LOGI("d3d11: capture (%s): frame trace + headset images (frame %ld)", g_captureReq ? "menu" : "key", G().frame);
        }
        g_captureReq = false;
        down = d;
    }
    if (g_traceState == 2) {
        { std::lock_guard<std::mutex> l(g_traceMx); char b[96]; snprintf(b, sizeof b, "---- Present (end of frame %ld), composited %d", G().frame, (int)g_f.composited); g_trace.push_back(b); }
        if (++g_traceFrames >= 3) { g_traceState = 3; FlushTrace(); }
    }
    if (g_traceState == 1) g_traceState = 2;
    g_s11.frames++;
    if (g_f.composited) { g_s11.stereo++; g_stereoTotal++; }
    g_s11.patched += g_f.patched; g_s11.right += g_f.right; g_s11.mirrored += g_f.mirrored; g_s11.hud += g_f.hud;
    g_f.uavBinds = g_uavBinds; g_uavBinds = 0;
    g_s11.dispatch += g_f.dispatch; g_s11.dispatchM += g_f.dispatchMirrored; g_s11.indirect += g_f.indirect; g_s11.uavBinds += g_f.uavBinds; g_s11.vsTwinSrv += g_f.vsTwinSrv; g_s11.sharedOut += g_f.sharedOut;
    g_lastStereo11 = g_f.composited;
    g_lastPatched11 = g_f.patched;
    bool patch = g_lastStereo11 || g_want11;
    g_want11 = false;
    g_f = Frame11();
    g_f.patch = patch;
    g_f.active = StereoOn11() && g_dev && xrb::ClientHostConnected() && StereoEyesBegin11();
    InterlockedIncrement(&G().frame);
}

// ---- swap chain
// ---- stage 2: the game image on the flat virtual screen in the headset (XrBridge, D3D11 keyed-mutex textures).
// Atlas per slot = [tiny left | tiny right | game image]; SLOT_MENU_MODE shows only the game-image region.
struct Ring11 {
    ID3D11Texture2D* tex[xrb::kSlots] = {};
    IDXGIKeyedMutex* km[xrb::kSlots] = {};
    ID3D11Texture2D* resolve = nullptr;   // MSAA back buffers are resolved first
    ID3D11Query* done = nullptr;          // slot-state mode: GPU finished the copy
    bool keyed = true;                    // keyed-mutex textures (else plain shared + SYNC_SLOT_STATE)
    UINT w = 0, h = 0, msaa = 1;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    int next = 0;
    bool failed = false;
    void Release() {
        for (int i = 0; i < xrb::kSlots; ++i) { if (km[i]) km[i]->Release(); if (tex[i]) tex[i]->Release(); km[i] = nullptr; tex[i] = nullptr; }
        if (resolve) resolve->Release();
        if (done) done->Release();
        resolve = nullptr; done = nullptr; w = h = 0;
    }
} g_ring11;
uint64_t g_vrSubmitted = 0, g_vrSkipped = 0;

bool EnsureRing11(const D3D11_TEXTURE2D_DESC& bb) {
    if (g_ring11.tex[0] && g_ring11.w == bb.Width && g_ring11.h == bb.Height && g_ring11.fmt == bb.Format) return true;
    if (g_ring11.failed && g_ring11.w == bb.Width && g_ring11.h == bb.Height) return false;
    g_ring11.Release();
    g_ring11.w = bb.Width; g_ring11.h = bb.Height; g_ring11.fmt = bb.Format; g_ring11.msaa = bb.SampleDesc.Count;
    DXGI_FORMAT f = bb.Format;
    bool rgba = f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_R8G8B8A8_TYPELESS;
    bool bgra = f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
                f == DXGI_FORMAT_B8G8R8X8_UNORM;
    if (!rgba && !bgra) {
        LOGW("d3d11 vr: back buffer format %d is not 8-bit RGBA/BGRA - no VR output (stage 2 copies only those)", (int)f);
        g_ring11.failed = true;
        return false;
    }
    // atlas = [left eye | right eye | game image], each back-buffer sized (eyes: stereo frames, game image: flat frames)
    const UINT W = bb.Width, H = bb.Height;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = 3 * W; d.Height = H; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = f == DXGI_FORMAT_B8G8R8X8_UNORM ? DXGI_FORMAT_B8G8R8A8_UNORM : f;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    // keyed-mutex shared textures first; some devices refuse them (BioShock Infinite) -> plain shared textures with
    // XrBridge's slot-state protocol and an event query for GPU completion
    for (int mode = 0; mode < 2; ++mode) {
        g_ring11.keyed = mode == 0;
        d.MiscFlags = g_ring11.keyed ? D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX : D3D11_RESOURCE_MISC_SHARED;
        ID3D11Texture2D* t = nullptr;
        HRESULT hr = g_dev->CreateTexture2D(&d, nullptr, &t);
        if (SUCCEEDED(hr)) { t->Release(); break; }
        LOGW("d3d11 vr: shared texture (%s) %ux%u format %d refused: %08lX", g_ring11.keyed ? "keyed mutex" : "plain", d.Width, d.Height,
             (int)d.Format, hr);
        if (mode == 1) { g_ring11.failed = true; return false; }
    }
    xrb::TextureSet ts;
    ts.atlasWidth = d.Width; ts.atlasHeight = d.Height;
    // the host wants the UNORM family code (it opens the shared texture with the matching view)
    ts.dxgiFormat = rgba ? (f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ? 29 : 28) : (f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ? 91 : 87);
    ts.sync = g_ring11.keyed ? xrb::SYNC_KEYED_MUTEX : xrb::SYNC_SLOT_STATE;
    ts.eyeRect[0] = {0, 0, (int)W, (int)H};
    ts.eyeRect[1] = {(int)W, 0, (int)W, (int)H};
    ts.hudRect = {(int)(2 * W), 0, (int)W, (int)H};
    for (int i = 0; i < xrb::kSlots; ++i) {
        IDXGIResource* r = nullptr;
        HANDLE sh = nullptr;
        HRESULT hr = g_dev->CreateTexture2D(&d, nullptr, &g_ring11.tex[i]);
        if (SUCCEEDED(hr) && g_ring11.keyed) hr = g_ring11.tex[i]->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&g_ring11.km[i]);
        if (SUCCEEDED(hr)) hr = g_ring11.tex[i]->QueryInterface(__uuidof(IDXGIResource), (void**)&r);
        if (SUCCEEDED(hr)) hr = r->GetSharedHandle(&sh);
        if (FAILED(hr)) {
            if (r) r->Release();
            LOGE("d3d11 vr: could not create shared texture %ux%u format %d: %08lX", d.Width, d.Height, (int)d.Format, hr);
            g_ring11.Release();
            g_ring11.w = bb.Width; g_ring11.h = bb.Height;
            g_ring11.failed = true;
            return false;
        }
        r->Release();
        ts.handles[i] = (uint64_t)(uintptr_t)sh;
    }
    if (bb.SampleDesc.Count > 1) {
        D3D11_TEXTURE2D_DESC rd = bb;
        rd.SampleDesc.Count = 1; rd.SampleDesc.Quality = 0; rd.BindFlags = 0; rd.MiscFlags = 0;
        if (FAILED(g_dev->CreateTexture2D(&rd, nullptr, &g_ring11.resolve))) g_ring11.resolve = nullptr;
    }
    if (!g_ring11.keyed) {
        D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
        g_dev->CreateQuery(&qd, &g_ring11.done);
    }
    xrb::ClientPublishTextures(ts);
    LOGI("d3d11 vr: ring of %d shared textures %ux%u (2 eyes + game image, %ux%u each, format %d)", xrb::kSlots, d.Width, d.Height, bb.Width,
         bb.Height, (int)d.Format);
    return true;
}

void UseSwapChainDevice(IDXGISwapChain* sc) {
    ID3D11Device* dev = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) || !dev) return;
    if (dev != g_dev) {
        if (g_dev) {   // a new device (resolution / renderer change)
            g_ring11.Release();
            ReleaseEyes();
            { Bypass11 bp; MenuShutdown11(); }
            {
                std::lock_guard<std::mutex> l(g_ctxMx);
                for (auto& kv : g_ctx) { for (auto& p : kv.second->patch) for (int e = 0; e < 2; ++e) if (p.second.b[e]) p.second.b[e]->Release(); delete kv.second; }
                g_ctx.clear();
                g_ctxGen++;
            }
            g_imm->Release(); g_dev->Release();
        }
        g_dev = dev;
        g_dev->AddRef();
        g_dev->GetImmediateContext(&g_imm);   // AddRef'd
        LOGI("d3d11: the game presents with device %p, immediate context %p", (void*)g_dev, (void*)g_imm);
    }
    dev->Release();
}

// Test aid (screenshot_interval > 0): the atlas just submitted (eyes + game image), quarter size, as a BMP in the run folder.

void DumpAtlas11(ID3D11Texture2D* atlas) {
    static DWORD next = 0;
    static int count = 0;
    DWORD now = GetTickCount();
    bool snap = g_snapLeft > 0 && G().frame >= g_snapNext;
    if (snap) { g_snapLeft--; g_snapNext = G().frame + 2; }
    else {
        if (G().s.screenshotInterval <= 0 || count >= G().s.screenshotMax || (next && (int)(now - next) < 0)) return;
        next = now + (DWORD)(G().s.screenshotInterval * 1000);
    }
    D3D11_TEXTURE2D_DESC d{};
    atlas->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.MiscFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(o_D11CreateTexture2D(g_dev, &d, nullptr, &st)) || !st) return;
    OC(g_imm, CopyResource)(g_imm, st, atlas);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(g_imm->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        bool bgr = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || d.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
        const UINT k = G().s.screenshotDiv >= 1 ? (UINT)G().s.screenshotDiv : 4, w = d.Width / k, h = d.Height / k, row = (w * 3 + 3) & ~3u;
        std::vector<uint8_t> px(row * h);
        for (UINT y = 0; y < h; ++y)
            for (UINT x = 0; x < w; ++x) {
                const uint8_t* s = (const uint8_t*)m.pData + (size_t)(y * k) * m.RowPitch + (size_t)x * k * 4;
                uint8_t* o = &px[(size_t)(h - 1 - y) * row + x * 3];
                o[0] = bgr ? s[0] : s[2]; o[1] = s[1]; o[2] = bgr ? s[2] : s[0];
            }
        g_imm->Unmap(st, 0);
        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%satlas11_%03d_f%06ld.bmp", G().runDir.c_str(), count++, G().frame);
        BITMAPFILEHEADER fh{};
        BITMAPINFOHEADER ih{};
        ih.biSize = sizeof(ih); ih.biWidth = (LONG)w; ih.biHeight = (LONG)h; ih.biPlanes = 1; ih.biBitCount = 24; ih.biSizeImage = (DWORD)px.size();
        fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + (DWORD)px.size();
        if (FILE* f = _wfopen(path, L"wb")) {
            fwrite(&fh, sizeof(fh), 1, f); fwrite(&ih, sizeof(ih), 1, f); fwrite(px.data(), 1, px.size(), f);
            fclose(f);
            LOGI("d3d11: atlas saved: %S (%s)", path, g_f.composited ? "stereo" : "flat");
        }
    }
    st->Release();
}

// The open menu, centred on the game image region of the atlas. The menu texture is RGBA; a BGRA atlas gets it
// through a converting draw-free path: none yet, so the menu is skipped there (logged once).
void CopyMenu(ID3D11Texture2D* atlas, UINT x0, UINT w, UINT h, DXGI_FORMAT f) {
    ID3D11Texture2D* m = MenuTexture11();
    if (!m) return;
    if (Typeless(f) != DXGI_FORMAT_R8G8B8A8_TYPELESS) {
        static bool warned = false;
        if (!warned) { warned = true; LOGW("d3d11: the VR menu needs an RGBA back buffer (this game: format %d) - not shown", (int)f); }
        return;
    }
    D3D11_TEXTURE2D_DESC md{};
    m->GetDesc(&md);
    if (md.Width > w || md.Height > h) return;
    OC(g_imm, CopySubresourceRegion)(g_imm, atlas, 0, x0 + (w - md.Width) / 2, (h - md.Height) / 2, 0, m, 0, nullptr);
}

// flat-screen panel (game image + VR menu) and the test atlas for screenshot_interval
ID3D11Texture2D* g_panel11 = nullptr; ID3D11Texture2D* g_panel11Resolve = nullptr; UINT g_panel11W = 0, g_panel11H = 0; DXGI_FORMAT g_panel11F = DXGI_FORMAT_UNKNOWN;
ID3D11Texture2D* g_testAtlas = nullptr; UINT g_testAtlasW = 0, g_testAtlasH = 0;
bool EnsurePanel11(const D3D11_TEXTURE2D_DESC& bd) {
    if (g_panel11 && g_panel11W == bd.Width && g_panel11H == bd.Height && g_panel11F == bd.Format) return true;
    if (g_panel11) { g_panel11->Release(); g_panel11 = nullptr; }
    if (g_panel11Resolve) { g_panel11Resolve->Release(); g_panel11Resolve = nullptr; }
    D3D11_TEXTURE2D_DESC d = bd;
    d.SampleDesc = {1, 0}; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; d.MiscFlags = 0; d.CPUAccessFlags = 0;
    d.MipLevels = 1; d.ArraySize = 1;
    if (d.Format == DXGI_FORMAT_B8G8R8X8_UNORM) d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    Bypass11 bp;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_panel11))) return false;
    if (bd.SampleDesc.Count > 1 && FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_panel11Resolve))) return false;
    g_panel11W = bd.Width; g_panel11H = bd.Height; g_panel11F = bd.Format;
    return true;
}
bool EnsureTestAtlas11(const D3D11_TEXTURE2D_DESC& bd) {
    if (g_testAtlas && g_testAtlasW == bd.Width && g_testAtlasH == bd.Height) return true;
    if (g_testAtlas) { g_testAtlas->Release(); g_testAtlas = nullptr; }
    D3D11_TEXTURE2D_DESC d{};
    d.Width = 3 * bd.Width; d.Height = bd.Height; d.MipLevels = 1; d.ArraySize = 1; d.Format = g_eyeTex[0] ? (DXGI_FORMAT)g_eyeFmt : bd.Format;
    d.SampleDesc = {1, 0}; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    Bypass11 bp;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_testAtlas))) return false;
    g_testAtlasW = bd.Width; g_testAtlasH = bd.Height;
    return true;
}



double FtNow();
// test aid sim_hmd_poses: walks the simulated headset through fixed poses (yaw,pitch[,cull factor]) once the game is in
// stereo, sim_hmd_pose_sec per pose, and saves one atlas [left | right | HUD] in the middle of each pose.
bool ApplyRenderScaleImpl();
static void SimPoseScript(float& pitchFixed, float& yawFixed) {
    static std::vector<std::array<float, 32>> poses; static float sweepYaw = -1, sweepPitch = 0; static bool parsed = false; static double t0 = 0; static int lastIx = -1, lastSnap = -1;
    if (!parsed) {
        parsed = true; const std::string& str = G().s.simHmdPoses; size_t i = 0;
        while (i < str.size()) {
            size_t e = str.find_first_of(";|", i); std::string it = str.substr(i, e == std::string::npos ? std::string::npos : e - i);
            std::array<float, 32> v{0, 0, -1, -1, -1, -1, -1, 999, 999, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1}; if (sscanf(it.c_str(), "%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9], &v[10], &v[11], &v[12], &v[13], &v[14], &v[15], &v[16], &v[17], &v[18], &v[19], &v[20], &v[21], &v[22], &v[23], &v[24], &v[25], &v[26], &v[27], &v[28], &v[29], &v[30], &v[31]) >= 2) poses.push_back(v);
            if (e == std::string::npos) break; i = e + 1;
        }
        if (!poses.empty()) LOGI("sim poses: %zu poses, %.1f s each", poses.size(), G().s.simHmdPoseSec);
    }
    static double first = 0; double now = FtNow() / 1000.0; if (first == 0) first = now;
    if (poses.empty() || g_stereoTotal < 200 || now - first < G().s.simHmdPoseStart) return;   // start in gameplay
    if (t0 == 0) t0 = now;
    double per = G().s.simHmdPoseSec > 1 ? G().s.simHmdPoseSec : 6;
    int ix = (int)((now - t0) / per);
    if (ix >= (int)poses.size()) return;   // done: back to the real pose
    yawFixed = poses[ix][0]; pitchFixed = poses[ix][1];
    if (ix != lastIx) {
        lastIx = ix; if (poses[ix][2] > 0) G().s.cullFov = poses[ix][2]; if (poses[ix][3] >= 0) G().s.listCbReupload = poses[ix][3] > 0.5f; if (poses[ix][4] >= 0) G().s.prevFramePose = poses[ix][4] > 0.5f; if (poses[ix][5] >= 0) G().s.gfTaaOff = poses[ix][5] > 0.5f; if (poses[ix][6] >= 0) G().s.cullAim = poses[ix][6] > 0.5f; if (poses[ix][7] != 999) G().s.cullAimTestYaw = poses[ix][7]; if (poses[ix][8] != 999) G().s.cullAimTestPitch = poses[ix][8];
        if (sweepYaw < 0) { sweepYaw = G().s.simHmdYaw; sweepPitch = G().s.simHmdPitch; }
        if (poses[ix][13] >= 0) FxPassSelect((int)poses[ix][13]);
        if (poses[ix][18] >= 0) { G().s.sharpen = poses[ix][18]; LOGI("sim poses: sharpen %.2f", G().s.sharpen); }
        if (poses[ix][31] >= 0) { G().s.layoutRepair = poses[ix][31] > 0.5f; LOGI("sim poses: layout_repair %d", (int)G().s.layoutRepair); }
        if (poses[ix][28] >= 0) { G().s.vrsSceneOnly = poses[ix][28] > 0.5f; LOGI("sim poses: vrs_scene_only %d", (int)G().s.vrsSceneOnly); }
        if (poses[ix][29] >= 0) { G().s.simGaze = poses[ix][29]; LOGI("sim poses: sim_gaze %.1f deg", G().s.simGaze); }
        if (poses[ix][30] > 0) { G().s.renderScale = poses[ix][30]; LOGI("sim poses: render_scale %.2f", G().s.renderScale); D3D11RenderScaleChanged(); }
        if (poses[ix][26] >= 0) { G().s.vrsStrength = (int)poses[ix][26]; LOGI("sim poses: vrs %d", G().s.vrsStrength); }
        if (poses[ix][27] >= 0) { G().s.vrsFullscreen = poses[ix][27] > 0.5f; LOGI("sim poses: vrs_fullscreen %d", (int)G().s.vrsFullscreen); }
        if (poses[ix][24] >= 0) { G().s.cineCull = poses[ix][24] > 0.5f; LOGI("sim poses: cine_cull_widen %d", (int)G().s.cineCull); }
        if (poses[ix][25] >= 0) { G().s.gameDof = poses[ix][25] > 0.5f; LOGI("sim poses: game_dof %d", (int)G().s.gameDof); }
        if (poses[ix][20] >= 0) { G().s.gfLodDist = (int)poses[ix][20]; LOGI("sim poses: game_lod_distance %d", G().s.gfLodDist); }
        if (poses[ix][21] >= 0) { G().s.testLeftOnly = poses[ix][21] > 0.5f; LOGI("sim poses: test_left_only %d", (int)G().s.testLeftOnly); }
        if (poses[ix][22] >= 0) { G().s.eyeMatCache = poses[ix][22] > 0.5f; LOGI("sim poses: eye_mat_cache %d", (int)G().s.eyeMatCache); }
        if (poses[ix][23] >= 0) { G().s.drawCamCache = poses[ix][23] > 0.5f; LOGI("sim poses: draw_cam_cache %d", (int)G().s.drawCamCache); }
        if (poses[ix][19] >= 0) { G().s.gfFxaa = (int)poses[ix][19]; LOGI("sim poses: game_fxaa %d", G().s.gfFxaa); }
        if (poses[ix][17] >= 0) { G().s.spinLock = poses[ix][17] > 0.5f; LOGI("sim poses: spin_lock %d", (int)G().s.spinLock); }
        if (poses[ix][16] >= 0) { G().s.lockFree = poses[ix][16] > 0.5f; LOGI("sim poses: lock_free %d", (int)G().s.lockFree); }
        if (poses[ix][15] >= 0) { G().s.skipBigCb = poses[ix][15] > 0.5f; LOGI("sim poses: skip_big_cb %d", (int)G().s.skipBigCb); }
        if (poses[ix][14] >= 0) { G().s.gfSsaoQuality = (int)poses[ix][14]; LOGI("sim poses: game_ssao_quality %d", G().s.gfSsaoQuality); }
        if (poses[ix][12] >= 0) { G().s.prevVpCopy = poses[ix][12] > 0.5f; LOGI("sim poses: prev_vp_copy %d", (int)G().s.prevVpCopy); }
        if (poses[ix][11] >= 0) { G().s.gfOcclusion = (int)poses[ix][11]; LOGI("sim poses: occlusion_mode %d", G().s.gfOcclusion); }
        if (poses[ix][10] >= 0) { G().s.stereoIsoMode = (int)poses[ix][10]; LOGI("sim poses: stereo_iso_mode %d", G().s.stereoIsoMode); }
        if (poses[ix][9] >= 0) { G().s.simHmdYaw = poses[ix][9] > 0.5f ? sweepYaw : 0; G().s.simHmdPitch = poses[ix][9] > 0.5f ? sweepPitch : 0; }
        LOGI("sim poses: pose %d yaw %.0f pitch %.0f cull_fov %.2f list_cb_reupload %d prev_frame_pose %d game_taa_off %d cull_aim %d test yaw %.0f pitch %.0f", ix, yawFixed, pitchFixed, G().s.cullFov, (int)G().s.listCbReupload, (int)G().s.prevFramePose, (int)G().s.gfTaaOff, (int)G().s.cullAim, G().s.cullAimTestYaw, G().s.cullAimTestPitch);
    }
    double inPose = now - t0 - ix * per;
    if (G().s.simHmdSnapSec > 0) {   // a series of snapshots from 1 s into the pose
        static double nextSnap = 0; static int snapPose = -1;
        if (snapPose != ix) { snapPose = ix; nextSnap = 1.0; }
        if (inPose >= nextSnap) { nextSnap += G().s.simHmdSnapSec; g_snapLeft = 1; g_snapNext = 0; }
    } else if (inPose > per * 0.6 && lastSnap != ix) { lastSnap = ix; g_snapLeft = 1; g_snapNext = 0; }
}

// ---- OpenXR thread. GreedFall presents from whichever job thread is free, so the OpenXR frame calls came from a
// different thread every frame. SteamVR's OpenXR runtime does not cope with that: xrWaitFrame stops waiting and its
// display times fall further and further behind, and the compositor shows only a few of the submitted frames
// (reproduced with xrbench, test 052: same symptoms, headset image frozen). All OpenXR calls now run on one thread of
// ours; the game thread hands the work over and waits, so the immediate context is still used by one thread at a time.
struct XrThread {
    HANDLE go = nullptr, done = nullptr, th = nullptr; std::function<void()> job;
    static DWORD WINAPI Main(LPVOID p) { auto* t = (XrThread*)p; for (;;) { WaitForSingleObject(t->go, INFINITE); { Bypass11 bp; t->job(); } SetEvent(t->done); } }
    void Run(std::function<void()> f) {
        if (!G().s.xrOneThread) { Bypass11 bp; f(); return; }
        if (!th) { go = CreateEventW(nullptr, FALSE, FALSE, nullptr); done = CreateEventW(nullptr, FALSE, FALSE, nullptr); th = CreateThread(nullptr, 1 << 20, Main, this, 0, nullptr);
                   if (!th) { Bypass11 bp; f(); return; } LOGI("xr: OpenXR calls run on their own thread (game presents from changing threads)"); }
        job = std::move(f); SetEvent(go); WaitForSingleObject(done, INFINITE); job = nullptr;
    }
} g_xrThread;

void VrFrame11(IDXGISwapChain* sc) {
    UseSwapChainDevice(sc);
    if (!g_dev || !g_imm) return;
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb) return;
    g_bbRes = bb;   // compared by address only (the swap chain keeps it alive)
    D3D11_TEXTURE2D_DESC bd{};
    bb->GetDesc(&bd);
    if (StereoOn11()) EnsureEyes(bd.Width, bd.Height, bd.Format);
    bool menuOpen = false;
    {   // VR settings menu (vrkit, D3D11 backend): its own texture, drawn with its own pipeline state
        Bypass11 bp;
        MenuInit11(g_dev, g_imm, bd.Width, bd.Height);
        menuOpen = MenuUpdate11();
    }
    // while the menu is open the headset shows the flat screen with the menu on it (readable, no stereo HUD layer yet)
    bool stereo = !menuOpen && g_f.composited && g_eyeTex[0] && g_eyeW == bd.Width && g_eyeH == bd.Height;
    if (G().s.vrEnabled && G().s.d3d11Vr) {
        VrStartClient();
        // in-process OpenXR (xr_inproc.cpp): eyes + panel straight into the runtime's swapchains on the game's device.
        // panel = the HUD over the stereo view, or the game image (with the VR menu on it) on the flat screen
        bool hudLayer = stereo && G().s.d3d11HudLayer && g_hudTex;
        ID3D11Texture2D* panel = hudLayer ? g_hudTex : nullptr;
        if (!stereo && EnsurePanel11(bd)) {
            ID3D11Resource* src = bb;
            if (bd.SampleDesc.Count > 1) { g_imm->ResolveSubresource(g_panel11Resolve, 0, bb, 0, bd.Format); src = g_panel11Resolve; }
            OC(g_imm, CopySubresourceRegion)(g_imm, g_panel11, 0, 0, 0, 0, src, 0, nullptr);
            if (menuOpen) CopyMenu(g_panel11, 0, bd.Width, bd.Height, bd.Format);
            panel = g_panel11;
        }
        VrsOffForMod(g_imm);   // the mod's own passes (sharpening, menu, copies) at full rate
        if (stereo && g_f.mainEmOk) vrs::Frame(g_dev, g_imm, g_eyeW, g_eyeH, g_f.mainEm.Pe[0], g_f.mainEm.Pe[1]);
        if (stereo) SharpenEyes11(g_imm);
        xrb::Submit11 sub;
        sub.stereo = stereo;
        sub.eye[0] = g_eyeTex[0]; sub.eye[1] = g_eyeTex[1];
        if (stereo) { int64_t t = 0; StereoPoses(sub.pose, sub.fov, &t); }
        sub.panel = panel;
        sub.panelIsHud = hudLayer;
        if ((G().s.screenshotInterval > 0 || g_snapLeft > 0) && EnsureTestAtlas11(bd)) {   // test aid: [left | right | panel] as before
            if (stereo) {
                OC(g_imm, CopySubresourceRegion)(g_imm, g_testAtlas, 0, 0, 0, 0, g_eyeTex[0], 0, nullptr);
                OC(g_imm, CopySubresourceRegion)(g_imm, g_testAtlas, 0, bd.Width, 0, 0, g_eyeTex[1], 0, nullptr);
            }
            if (panel) OC(g_imm, CopySubresourceRegion)(g_imm, g_testAtlas, 0, 2 * bd.Width, 0, 0, panel, 0, nullptr);
            DumpAtlas11(g_testAtlas);
        }
        {
            Bypass11 bp;   // our own copies into the swapchains are not game calls
            ProbeTex(0, "game back buffer", g_dev, g_imm, bb);
            if (stereo) ProbeTex(1, "left eye image", g_dev, g_imm, g_eyeTex[0]);
            g_xrThread.Run([&] { xrb::XrFrame11(g_dev, g_imm, sub); });
        }
        if (xrb::ClientHostConnected()) g_vrSubmitted++; else g_vrSkipped++;
    }
    bb->Release();
}

// ---- frame timing (always on, logged with the status every 10 s): where a frame's time goes
struct FrameTiming {
    double qpf = 0; LARGE_INTEGER last{};
    double sum = 0, mx = 0, vr = 0, present = 0, begin = 0; int n = 0, slow = 0, syncForced = 0;
    // GPU frame time: timestamp at each Present, read back 4 frames later
    ID3D11Query* disj[4] = {}; ID3D11Query* ts[4] = {}; int qi = 0; bool qOpen = false;
    ID3D11Query* tsS[4] = {}; bool sIssued[4] = {}; double busySum = 0, busyMax = 0; int busyN = 0;   // GPU busy: first game work .. Present
    double gpuSum = 0, gpuMax = 0; int gpuN = 0;
} g_ft;
double FtNow() { LARGE_INTEGER t; QueryPerformanceCounter(&t); if (!g_ft.qpf) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_ft.qpf = (double)f.QuadPart; } return t.QuadPart * 1000.0 / g_ft.qpf; }
// ---- GPU queue limit. GreedFall's CPU can run several frames ahead of the GPU (Present never blocks with vsync off;
// on the user's PC the 4-frames-later timestamp was not ready for ~40% of frames). Every frame then reaches SteamVR long
// after the display time it was submitted for (SteamVR's predicted times were 200-300 ms in the past): head tracking stays
// smooth (re-projection) but the game content shown jumps ("NPCs vanish and appear a step ahead").
// After each Present: an event query marks the end of the frame on the GPU; with gpu_queue_limit = N the CPU waits until
// the frame N back has finished on the GPU before it starts the next one. Logged: queue depth and wait time.
struct GpuQueue { ID3D11Query* ev[16] = {}; long frame[16] = {}; int head = 0; double waitMs = 0; int depthSum = 0, depthMax = 0, n = 0, timeouts = 0; } g_gq;
void GpuQueueLimit() {
    if (!g_dev || !g_imm) return;
    Bypass11 bp;
    int i = g_gq.head % 16;
    if (!g_gq.ev[i]) { D3D11_QUERY_DESC d{D3D11_QUERY_EVENT, 0}; if (FAILED(g_dev->CreateQuery(&d, &g_gq.ev[i]))) return; }
    g_imm->End(g_gq.ev[i]);
    g_gq.frame[i] = G().frame;
    g_gq.head++;
    // queue depth: frames submitted but not finished on the GPU (newest first)
    int depth = 0;
    for (int k = 0; k < 15 && k < g_gq.head; ++k) {
        int j = (g_gq.head - 1 - k) % 16;
        BOOL done = FALSE;
        if (g_imm->GetData(g_gq.ev[j], &done, sizeof done, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && done) break;
        depth++;
    }
    g_gq.depthSum += depth; g_gq.depthMax = std::max(g_gq.depthMax, depth); g_gq.n++;
    int limit = G().s.gpuQueueLimit;
    if (limit <= 0 || g_gq.head <= limit) return;
    int j = (g_gq.head - 1 - limit) % 16;   // the frame `limit` back must be finished
    LARGE_INTEGER f, t0, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    g_imm->Flush();
    for (;;) {
        BOOL done = FALSE;
        if (g_imm->GetData(g_gq.ev[j], &done, sizeof done, 0) == S_OK && done) break;
        QueryPerformanceCounter(&t);
        if ((t.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart > 200) { g_gq.timeouts++; break; }
        SwitchToThread();
    }
    QueryPerformanceCounter(&t);
    g_gq.waitMs += (t.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
}
std::string GpuQueueLine() {
    char b[200];
    snprintf(b, sizeof b, "GPU queue: %.1f frames avg / %d max behind the CPU at Present; limit %d, waited %.1f ms per frame (%d timeouts)",
             g_gq.n ? (double)g_gq.depthSum / g_gq.n : 0.0, g_gq.depthMax, G().s.gpuQueueLimit, g_gq.n ? g_gq.waitMs / g_gq.n : 0.0, g_gq.timeouts);
    g_gq.depthSum = g_gq.depthMax = g_gq.n = g_gq.timeouts = 0; g_gq.waitMs = 0;
    return b;
}
void GpuFrameMark() {   // called at each Present: closes the previous frame's disjoint query, opens this one
    if (!g_dev || !g_imm) return;
    Bypass11 bp;
    int i = g_ft.qi & 3;
    if (!g_ft.disj[0]) {
        D3D11_QUERY_DESC d{D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, t{D3D11_QUERY_TIMESTAMP, 0};
        for (int k = 0; k < 4; ++k) { g_dev->CreateQuery(&d, &g_ft.disj[k]); g_dev->CreateQuery(&t, &g_ft.ts[k]); g_dev->CreateQuery(&t, &g_ft.tsS[k]); }
        if (!g_ft.disj[3] || !g_ft.ts[3]) return;
    }
    // read the oldest one (4 frames back) without waiting
    int old = (g_ft.qi + 1) & 3;
    if (g_ft.qi >= 4) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{}; UINT64 a = 0, b = 0;
        int prevOfOld = (old + 3) & 3;   // timestamp of the frame before = start of that frame
        if (g_imm->GetData(g_ft.disj[old], &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && !dj.Disjoint && dj.Frequency &&
            g_imm->GetData(g_ft.ts[old], &b, sizeof b, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
            g_imm->GetData(g_ft.ts[prevOfOld], &a, sizeof a, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && b > a) {
            double ms = (b - a) * 1000.0 / dj.Frequency;
            if (ms < 1000) { g_ft.gpuSum += ms; g_ft.gpuMax = std::max(g_ft.gpuMax, ms); g_ft.gpuN++; }
            UINT64 s0 = 0;
            if (g_ft.sIssued[old] && g_ft.tsS[old] && g_imm->GetData(g_ft.tsS[old], &s0, sizeof s0, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && b > s0) {
                double bm = (b - s0) * 1000.0 / dj.Frequency;
                if (bm < 1000) { g_ft.busySum += bm; g_ft.busyMax = std::max(g_ft.busyMax, bm); g_ft.busyN++; }
            }
        }
    }
    if (g_ft.qOpen) g_imm->End(g_ft.disj[(g_ft.qi + 3) & 3]);
    g_imm->End(g_ft.ts[i]);      // GPU reached this Present = end of the frame
    g_imm->Begin(g_ft.disj[i]);  // (disjoint brackets the next frame)
    g_ft.qOpen = true;
    g_ft.qi++;
    g_ft.sIssued[g_ft.qi & 3] = false;   // next frame: its first game work issues the start timestamp
}
// GPU busy time: timestamp at the frame's first game work on the immediate context (draw or command list), so the GPU's
// idle time while the frame is paced (xrWaitFrame) does not count. "GPU busy" in the timing line.
void GpuStartMark(ID3D11DeviceContext* c) {
    if (c != g_imm || !g_ft.tsS[0]) return;
    int i = g_ft.qi & 3;
    if (g_ft.sIssued[i]) return;
    g_ft.sIssued[i] = true;
    Bypass11 bp;
    g_imm->End(g_ft.tsS[i]);
}
const char* FrameTimingLine() {
    static char b[400];
    if (!g_ft.n) return "no frames";
    snprintf(b, sizeof b, "CPU frame %.1f ms avg / %.1f max, %d of %d frames > 1.5x avg; VR submit %.1f ms, game Present %.1f ms, frame start (xrWaitFrame) %.1f ms; "
             "GPU Present-to-Present %.1f ms avg / %.1f max (%d samples), GPU busy %.2f ms avg / %.1f max; desktop vsync forced off on %d frames",
             g_ft.sum / g_ft.n, g_ft.mx, g_ft.slow, g_ft.n, g_ft.vr / g_ft.n, g_ft.present / g_ft.n, g_ft.begin / g_ft.n, g_ft.gpuN ? g_ft.gpuSum / g_ft.gpuN : 0.0, g_ft.gpuMax,
             g_ft.gpuN, g_ft.busyN ? g_ft.busySum / g_ft.busyN : 0.0, g_ft.busyMax, g_ft.syncForced);
    return b;
}
std::vector<double> g_ftWin;   // this period's intervals (for the slow-frame count)

HRESULT STDMETHODCALLTYPE Hk_Present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    double tA = FtNow();
    if (!(flags & DXGI_PRESENT_TEST)) {
        if (g_ft.last.QuadPart) { double dt = tA - g_ft.last.QuadPart / 1000.0; g_ft.sum += dt; g_ft.mx = std::max(g_ft.mx, dt); g_ft.n++; g_ftWin.push_back(dt); }
        g_ft.last.QuadPart = (LONGLONG)(tA * 1000.0);
        GpuFrameMark();
        VrFrame11(sc);
    }
    double tB = FtNow();
    g_ft.vr += tB - tA;
    // The headset paces the game (xrWaitFrame); the desktop monitor's vsync on top of that makes two clocks (60 / 90 Hz)
    // beat against each other: uneven, long frames. desktop_vsync = 0 (default): no vsync while the headset shows the game.
    if (sync && G().s.desktopVsyncOff && xrb::ClientHostVisible()) { sync = 0; g_ft.syncForced++; }
    GreedfallTick();
    g_st.frames++;
    DWORD now = GetTickCount();
    if (!g_lastLog) g_lastLog = now;
    if (now - g_lastLog >= 10000) {
        double fr = (double)(g_st.frames - g_last.frames);
        if (fr > 0)
            if (g_mainFovMax > 0) { LOGI("d3d11: main camera (as rendered) FOV %.1f x %.1f deg, vertical %.1f..%.1f this period", g_f.mainCam.fovH, g_f.mainCam.fovV, g_mainFovMin, g_mainFovMax); g_mainFovMin = 1e9f; g_mainFovMax = 0; }
            LOGI("d3d11 status: %.1f fps, %.0f draws/frame (%.0f on deferred contexts, %u contexts, %.1f command lists/frame; %.0f with a camera found), "
                 "%.0f constant-buffer updates + %.0f maps/frame, "
                 "%.0f render-target changes/frame; %u constant buffers, %u vertex shaders (%u with a camera)",
                 fr * 1000.0 / (now - g_lastLog), (g_st.draws - g_last.draws) / fr, (g_st.deferredDraws - g_last.deferredDraws) / fr, g_deferredCtx,
                 (g_st.cmdLists - g_last.cmdLists) / fr, (g_st.camDraws - g_last.camDraws) / fr,
                 (g_st.cbUpdates - g_last.cbUpdates) / fr, (g_st.cbMaps - g_last.cbMaps) / fr, (g_st.rtSets - g_last.rtSets) / fr,
                 (unsigned)g_cb.size(), (unsigned)g_vsHash.size(), [] { unsigned n = 0; for (auto& kv : g_vsInfo) n += kv.second.camDraws > 0; return n; }());
        {
            LARGE_INTEGER fq; QueryPerformanceFrequency(&fq); double ms = 1000.0 / (double)fq.QuadPart / (fr > 0 ? fr : 1);
            LONG64 w = InterlockedExchange64(&g_pfWait, 0), h = InterlockedExchange64(&g_pfHeld, 0), k = InterlockedExchange64(&g_pfLocks, 0),
                   hk = InterlockedExchange64(&g_pfHook, 0), dr = InterlockedExchange64(&g_pfDraw, 0), cp = InterlockedExchange64(&g_pfCopied, 0),
                   sk = InterlockedExchange64(&g_pfSkipped, 0);
            LOGI("d3d11 perf per frame (all threads summed): lock wait %.2f ms, lock held %.2f ms (%.0f locks), map/unmap/update hooks %.2f ms, "
                 "draw path %.2f ms (incl. the game's draw calls), buffer copies %.2f MB (+%.2f MB skipped, big buffers %s)",
                 w * ms, h * ms, k / (fr > 0 ? fr : 1), hk * ms, dr * ms, cp / 1048576.0 / (fr > 0 ? fr : 1), sk / 1048576.0 / (fr > 0 ? fr : 1),
                 G().s.skipBigCb ? "not copied" : "copied");
            {
                LONG64 ob = InterlockedExchange64(&g_pfObs, 0), pa = InterlockedExchange64(&g_pfPatch, 0), gl = InterlockedExchange64(&g_pfGameL, 0),
                       gr = InterlockedExchange64(&g_pfGameR, 0), bi = InterlockedExchange64(&g_pfBind, 0), on = InterlockedExchange64(&g_pfObsN, 0),
                       oh = InterlockedExchange64(&g_pfObsHit, 0), ec = InterlockedExchange64(&g_pfEmCalc, 0), eh = InterlockedExchange64(&g_pfEmHit, 0);
                static ULONGLONG lastCpu = 0; static double lastWall = 0;
                FILETIME a, b2, kt, ut; ULONGLONG cpu = 0;
                if (GetProcessTimes(GetCurrentProcess(), &a, &b2, &kt, &ut))
                    cpu = (((ULONGLONG)kt.dwHighDateTime << 32) | kt.dwLowDateTime) + (((ULONGLONG)ut.dwHighDateTime << 32) | ut.dwLowDateTime);
                double wall = FtNow();
                double cpuMs = lastCpu ? (cpu - lastCpu) / 10000.0 : 0, wallMs = lastWall > 0 ? wall - lastWall : 0;
                SYSTEM_INFO si{}; GetSystemInfo(&si);
                LOGI("d3d11 perf split per frame: camera check %.2f ms (%.0f%% reused of %.0f), eye cameras %.2f ms (%.0f computed, %.0f reused), "
                     "game draws left %.2f ms, right %.2f ms, eye switching %.2f ms; process CPU %.1f ms per frame (%.0f%% of %u cores)%s%s; VRS %s on %.0f of %.0f draws",
                     ob * ms, on ? 100.0 * oh / on : 0.0, on / (fr > 0 ? fr : 1), pa * ms, ec / (fr > 0 ? fr : 1), eh / (fr > 0 ? fr : 1), gl * ms, gr * ms, bi * ms,
                     fr > 0 ? cpuMs / fr : 0.0, wallMs > 0 ? 100.0 * cpuMs / wallMs / si.dwNumberOfProcessors : 0.0, (unsigned)si.dwNumberOfProcessors,
                     G().s.testLeftOnly ? ", TEST: right eye not drawn" : "", G().s.drawCamCache && G().s.eyeMatCache ? "" : ", a CPU cache is off",
                     vrs::g_state > 0 ? (G().s.vrsStrength > 0 ? "level " + std::to_string(G().s.vrsStrength) : std::string("off")).c_str() : "unavailable",
                     InterlockedExchange64(&vrs::g_draws, 0) / (double)(fr > 0 ? fr : 1), InterlockedExchange64(&vrs::g_drawsAll, 0) / (double)(fr > 0 ? fr : 1));
                lastCpu = cpu; lastWall = wall;
            }
            static LONG lastGen = 0; LONG gen = g_cbGen;
            LOGI("d3d11 perf: %.1f constant buffers created per frame, lock-free buffer paths %s", (gen - lastGen) / (fr > 0 ? fr : 1), G().s.lockFree ? "on" : "off");
            lastGen = gen;
        }
        static uint64_t lastSub = 0, lastSkip = 0;
        LOGI("d3d11 vr: %llu frames to the headset, %llu skipped (slot busy), host %s", (unsigned long long)(g_vrSubmitted - lastSub),
             (unsigned long long)(g_vrSkipped - lastSkip), xrb::ClientHostVisible() ? "visible" : xrb::ClientHostConnected() ? "connected" : "not connected");
        lastSub = g_vrSubmitted; lastSkip = g_vrSkipped;
        if (StereoOn11()) {
            double n = g_s11.frames ? (double)g_s11.frames : 1.0;
            LOGI("d3d11 stereo: %llu of %llu frames in stereo; per frame %.0f patched, %.0f right-eye draws, %.0f mirrored clears/copies, %.0f UI "
                 "draws into the eyes; %u twins", (unsigned long long)g_s11.stereo, (unsigned long long)g_s11.frames, g_s11.patched / n,
                 g_s11.right / n, g_s11.mirrored / n, g_s11.hud / n, g_twins11);
            LOGI("d3d11 stereo diag per frame: %.1f dispatches (%.1f mirrored), %.1f indirect/auto draws, %.1f UAV binds, %.1f VS twinned-SRV binds, "
                 "%.1f draws into shared targets reading twins", g_s11.dispatch / n, g_s11.dispatchM / n, g_s11.indirect / n, g_s11.uavBinds / n, g_s11.vsTwinSrv / n,
                 g_s11.sharedOut / n);
            g_s11 = Stats11();
        }
        LOGI("greedfall: %s", GreedfallStatus());
        if (g_ft.n) {
            double avg = g_ft.sum / g_ft.n;
            for (double d : g_ftWin) if (d > 1.5 * avg) g_ft.slow++;
            LOGI("timing: %s", FrameTimingLine());
            LOGI("lists: %s", ListStatsLine().c_str());
            LOGI("%s", GpuQueueLine().c_str());
            { std::lock_guard<SpinMx> lu(g_mx); LOGI("unpatched 3D eye draws (head-locked): %s", UnpatchedLine().c_str()); }
        }
        { ID3D11Query* dj[4]; ID3D11Query* t[4]; ID3D11Query* ts0[4]; bool si[4]; memcpy(dj, g_ft.disj, sizeof dj); memcpy(t, g_ft.ts, sizeof t); memcpy(ts0, g_ft.tsS, sizeof ts0); memcpy(si, g_ft.sIssued, sizeof si); int qi = g_ft.qi; bool qo = g_ft.qOpen; double qpf = g_ft.qpf; LARGE_INTEGER l = g_ft.last;
          g_ft = FrameTiming(); memcpy(g_ft.disj, dj, sizeof dj); memcpy(g_ft.ts, t, sizeof t); memcpy(g_ft.tsS, ts0, sizeof ts0); memcpy(g_ft.sIssued, si, sizeof si); g_ft.qi = qi; g_ft.qOpen = qo; g_ft.qpf = qpf; g_ft.last = l; g_ftWin.clear(); }
        g_last = g_st;
        g_lastLog = now;
    }
    double tC = FtNow();
    HRESULT hr = o_DxgiPresent(sc, sync, flags);
    g_ft.present += FtNow() - tC;
    if (!(flags & DXGI_PRESENT_TEST)) {
        double tD = FtNow();
        GpuQueueLimit();   // before the next frame starts: the GPU may be at most gpu_queue_limit frames behind
        { Bypass11 bp; D3D11RenderScaleTick(); xrb::XrSetFixLateTime(G().s.xrFixLateTime); float spf = G().s.simHmdPitchFixed, syf = 0; SimPoseScript(spf, syf); xrb::XrSetSimulatedHead(G().s.simHmdPitch, G().s.simHmdYaw, G().s.simHmdPeriod, spf, syf); g_xrThread.Run([] { xrb::XrBeginFrame11(G().s.xrStartPacing); }); }   // the next frame starts here: headset pacing + its pose
        g_ft.begin += FtNow() - tD;
        EndFrame11();
    }
    return hr;
}


// ---- Render resolution: the game renders at the headset's recommended per-eye size x render_scale (or render_width /
//      render_height) instead of its own 16:9 resolution. GreedFall sizes its scene targets from the swap chain, so the
//      swap chain is created (and resized) at that size; the window keeps its own size (DXGI scales the desktop view).
//      d3d11_headset_res = 0 (menu Resolution) keeps the game's resolution. Decided once, before the device exists.
static UINT g_forceW = 0, g_forceH = 0;
static bool g_forceDecided = false;
static void DecideRenderSize(UINT gameW, UINT gameH) {
    if (g_forceDecided) return;
    g_forceDecided = true;
    const Settings& s = G().s;
    if (!s.vrEnabled || !s.d3d11Stereo || !s.d3d11HeadsetRes) { LOGI("render size: game resolution %ux%u (headset resolution off)", gameW, gameH); return; }
    UINT w = 0, h = 0;
    const char* src = "";
    if (s.renderWidth > 0 && s.renderHeight > 0) { w = s.renderWidth; h = s.renderHeight; src = "render_width/height"; }
    else if (s.renderScale > 0) {
        VrStartClient();
        unsigned rw = 0, rh = 0;
        DWORD t0 = GetTickCount();
        if (xrb::ClientWaitRecommendedSize(&rw, &rh, 20000)) {
            w = (UINT)lroundf(rw * s.renderScale); h = (UINT)lroundf(rh * s.renderScale); src = "headset";
            LOGI("render size: headset recommends %ux%u per eye, render_scale %.2f (waited %lu ms)", rw, rh, s.renderScale, GetTickCount() - t0);
        } else LOGW("render size: no headset size after 20 s - using the game's resolution");
    }
    if (!w || !h) { LOGI("render size: game resolution %ux%u", gameW, gameH); return; }
    const UINT kMax = 8192;
    if (w > kMax) { h = (UINT)((double)h * kMax / w); w = kMax; }
    if (h > kMax) { w = (UINT)((double)w * kMax / h); h = kMax; }
    w &= ~1u; h &= ~1u;
    g_forceW = w; g_forceH = h;
    LOGI("render size: the game renders %ux%u per eye (from %s; its own resolution %ux%u)", w, h, src, gameW, gameH);
}
// Live render_scale: new forced size, then the game window is resized to it; GreedFall (windowed) answers with
// ResizeBuffers and recreates its targets at the new size (the eye textures follow the back buffer).
static HWND g_scHwnd = nullptr;
bool ApplyRenderScaleImpl() {
    const Settings& s = G().s;
    if (!g_forceW || !g_scHwnd) { LOGW("render size: the headset resolution is not in use - restart the game to apply"); return false; }
    unsigned rw = 0, rh = 0;
    if (!xrb::ClientWaitRecommendedSize(&rw, &rh, 100)) return false;
    UINT w = (UINT)lroundf(rw * s.renderScale), h = (UINT)lroundf(rh * s.renderScale);
    const UINT kMax = 8192;
    if (w > kMax) { h = (UINT)((double)h * kMax / w); w = kMax; }
    if (h > kMax) { w = (UINT)((double)w * kMax / h); h = kMax; }
    w &= ~1u; h &= ~1u;
    if (w < 256 || h < 256) return false;
    g_forceW = w; g_forceH = h;
    RECT r{0, 0, (LONG)w, (LONG)h};
    DWORD st = (DWORD)GetWindowLongPtrW(g_scHwnd, GWL_STYLE), ex = (DWORD)GetWindowLongPtrW(g_scHwnd, GWL_EXSTYLE);
    AdjustWindowRectEx(&r, st, FALSE, ex);
    BOOL ok = SetWindowPos(g_scHwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    LOGI("render size: render_scale %.2f -> %ux%u per eye now (window resize %s)", s.renderScale, w, h, ok ? "sent" : "FAILED");
    return ok != FALSE;
}
void D3D11RenderSizeImpl(UINT gameW, UINT gameH, UINT* w, UINT* h) { DecideRenderSize(gameW, gameH); *w = g_forceW; *h = g_forceH; }
static void ForceSize(UINT& w, UINT& h, const char* what) {
    if (!g_forceW) return;
    if (w != g_forceW || h != g_forceH) LOGI("render size: %s %ux%u -> %ux%u", what, w, h, g_forceW, g_forceH);
    w = g_forceW; h = g_forceH;
}
typedef HRESULT(STDMETHODCALLTYPE* DxgiResize_t)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
static DxgiResize_t o_DxgiResize = nullptr;
HRESULT STDMETHODCALLTYPE Hk_ResizeBuffers(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags) {
    if (g_forceW) ForceSize(w, h, "ResizeBuffers");   // (0 x 0 = window size: forced as well)
    return o_DxgiResize(sc, n, w, h, f, flags);
}
void HookSwapChain(IDXGISwapChain* sc) {
    static bool done = false;
    if (done || !sc) return;
    done = true;
    DXGI_SWAP_CHAIN_DESC d{};
    sc->GetDesc(&d);
    LOGI("d3d11: swap chain %ux%u format %d, %u buffers, windowed %d, swap effect %d", d.BufferDesc.Width, d.BufferDesc.Height,
         (int)d.BufferDesc.Format, d.BufferCount, d.Windowed, (int)d.SwapEffect);
    g_scHwnd = d.OutputWindow;
    o_DxgiPresent = (DxgiPresent_t)HookVtable(sc, VT_DXGISC_Present, (void*)&Hk_Present);
    o_DxgiResize = (DxgiResize_t)HookVtable(sc, VT_DXGISC_ResizeBuffers, (void*)&Hk_ResizeBuffers);
}

HRESULT STDMETHODCALLTYPE Hk_CreateSwapChain(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** out) {
    DXGI_SWAP_CHAIN_DESC dd{};
    if (d) { dd = *d; DecideRenderSize(dd.BufferDesc.Width, dd.BufferDesc.Height); ForceSize(dd.BufferDesc.Width, dd.BufferDesc.Height, "CreateSwapChain"); d = &dd; }
    HRESULT hr = o_DxgiCreateSwapChain(f, dev, d, out);
    if (SUCCEEDED(hr) && out) HookSwapChain(*out);
    return hr;
}
HRESULT STDMETHODCALLTYPE Hk_CreateSwapChainForHwnd(IDXGIFactory2* f, IUnknown* dev, HWND w, const DXGI_SWAP_CHAIN_DESC1* d,
                                                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* o, IDXGISwapChain1** out) {
    DXGI_SWAP_CHAIN_DESC1 dd{};
    if (d) { dd = *d; DecideRenderSize(dd.Width, dd.Height); ForceSize(dd.Width, dd.Height, "CreateSwapChainForHwnd"); d = &dd; }
    HRESULT hr = o_DxgiCreateSwapChainForHwnd(f, dev, w, d, fs, o, out);
    if (SUCCEEDED(hr) && out) HookSwapChain(*out);
    return hr;
}
void HookFactory(void* f) {
    if (!f) return;
    if (!o_DxgiCreateSwapChain) o_DxgiCreateSwapChain = (DxgiCreateSwapChain_t)HookVtable(f, VT_DXGIF_CreateSwapChain, (void*)&Hk_CreateSwapChain);
    IDXGIFactory2* f2 = nullptr;
    if (!o_DxgiCreateSwapChainForHwnd && SUCCEEDED(((IUnknown*)f)->QueryInterface(__uuidof(IDXGIFactory2), (void**)&f2)) && f2) {
        o_DxgiCreateSwapChainForHwnd = (DxgiCreateSwapChainForHwnd_t)HookVtable(f2, VT_DXGIF2_CreateSwapChainForHwnd, (void*)&Hk_CreateSwapChainForHwnd);
        f2->Release();
    }
}

// UE3 creates and releases several probe devices before the real one: the vtables are shared (hooked once from any
// of them), but the device / context used for output are taken from the swap chain at Present (UseSwapChainDevice).
void HookDevice(ID3D11Device* dev, ID3D11DeviceContext* imm, D3D_FEATURE_LEVEL fl) {
    static bool hooked = false;
    if (!dev || hooked) return;
    hooked = true;
    LOGI("d3d11: device created by the game, feature level %x - UEVR3 D3D11 (stereo %s)", (unsigned)fl, StereoOn11() ? "on" : "off: flat screen");
#define H(obj, name) o_D11##name = (D11##name##_t)HookVtable(obj, VT_D11##obj##_##name, (void*)&Hk_##name);
    {
        void* DEV = dev; (void)DEV;
        o_D11CreateBuffer = (D11CreateBuffer_t)HookVtable(dev, VT_D11DEV_CreateBuffer, (void*)&Hk_CreateBuffer);
        o_D11CreateVertexShader = (D11CreateVertexShader_t)HookVtable(dev, VT_D11DEV_CreateVertexShader, (void*)&Hk_CreateVertexShader);
        o_D11CreatePixelShader = (D11CreatePixelShader_t)HookVtable(dev, VT_D11DEV_CreatePixelShader, (void*)&Hk_CreatePixelShader);
        o_D11CreateTexture2D = (D11CreateTexture2D_t)HookVtable(dev, VT_D11DEV_CreateTexture2D, (void*)&Hk_CreateTexture2D);
        o_D11CreateRenderTargetView = (D11CreateRenderTargetView_t)HookVtable(dev, VT_D11DEV_CreateRenderTargetView, (void*)&Hk_CreateRenderTargetView);
        o_D11CreateShaderResourceView = (D11CreateShaderResourceView_t)HookVtable(dev, VT_D11DEV_CreateShaderResourceView, (void*)&Hk_CreateShaderResourceView);
        o_D11CreateDepthStencilView = (D11CreateDepthStencilView_t)HookVtable(dev, VT_D11DEV_CreateDepthStencilView, (void*)&Hk_CreateDepthStencilView);
        o_D11CreateUnorderedAccessView = (D11CreateUnorderedAccessView_t)HookVtable(dev, VT_D11DEV_CreateUnorderedAccessView, (void*)&Hk_CreateUnorderedAccessView);
        o_D11CreateDeferredContext = (D11CreateDeferredContext_t)HookVtable(dev, VT_D11DEV_CreateDeferredContext, (void*)&Hk_CreateDeferredContext);
    }
    if (imm) {
        o_D11VSSetConstantBuffers = (D11VSSetConstantBuffers_t)HookVtable(imm, VT_D11CTX_VSSetConstantBuffers, (void*)&Hk_VSSetConstantBuffers);
        o_D11HSSetConstantBuffers = (D11HSSetConstantBuffers_t)HookVtable(imm, VT_D11CTX_HSSetConstantBuffers, (void*)&Hk_HSSetConstantBuffers);
        o_D11DSSetConstantBuffers = (D11DSSetConstantBuffers_t)HookVtable(imm, VT_D11CTX_DSSetConstantBuffers, (void*)&Hk_DSSetConstantBuffers);
        o_D11GSSetConstantBuffers = (D11GSSetConstantBuffers_t)HookVtable(imm, VT_D11CTX_GSSetConstantBuffers, (void*)&Hk_GSSetConstantBuffers);
        o_D11VSSetShader = (D11VSSetShader_t)HookVtable(imm, VT_D11CTX_VSSetShader, (void*)&Hk_VSSetShader);
        o_D11Draw = (D11Draw_t)HookVtable(imm, VT_D11CTX_Draw, (void*)&Hk_Draw);
        o_D11DrawIndexed = (D11DrawIndexed_t)HookVtable(imm, VT_D11CTX_DrawIndexed, (void*)&Hk_DrawIndexed);
        o_D11DrawInstanced = (D11DrawInstanced_t)HookVtable(imm, VT_D11CTX_DrawInstanced, (void*)&Hk_DrawInstanced);
        o_D11DrawIndexedInstanced = (D11DrawIndexedInstanced_t)HookVtable(imm, VT_D11CTX_DrawIndexedInstanced, (void*)&Hk_DrawIndexedInstanced);
        o_D11Map = (D11Map_t)HookVtable(imm, VT_D11CTX_Map, (void*)&Hk_Map);
        o_D11Unmap = (D11Unmap_t)HookVtable(imm, VT_D11CTX_Unmap, (void*)&Hk_Unmap);
        o_D11UpdateSubresource = (D11UpdateSubresource_t)HookVtable(imm, VT_D11CTX_UpdateSubresource, (void*)&Hk_UpdateSubresource);
        o_D11OMSetRenderTargets = (D11OMSetRenderTargets_t)HookVtable(imm, VT_D11CTX_OMSetRenderTargets, (void*)&Hk_OMSetRenderTargets);
        o_D11OMSetRenderTargetsAndUnorderedAccessViews = (D11OMSetRenderTargetsAndUnorderedAccessViews_t)HookVtable(
            imm, VT_D11CTX_OMSetRenderTargetsAndUnorderedAccessViews, (void*)&Hk_OMSetRenderTargetsAndUnorderedAccessViews);
        o_D11PSSetShaderResources = (D11PSSetShaderResources_t)HookVtable(imm, VT_D11CTX_PSSetShaderResources, (void*)&Hk_PSSetShaderResources);
        o_D11PSSetConstantBuffers = (D11PSSetConstantBuffers_t)HookVtable(imm, VT_D11CTX_PSSetConstantBuffers, (void*)&Hk_PSSetConstantBuffers);
        o_D11PSSetShader = (D11PSSetShader_t)HookVtable(imm, VT_D11CTX_PSSetShader, (void*)&Hk_PSSetShader);
        o_D11ClearRenderTargetView = (D11ClearRenderTargetView_t)HookVtable(imm, VT_D11CTX_ClearRenderTargetView, (void*)&Hk_ClearRenderTargetView);
        o_D11ClearDepthStencilView = (D11ClearDepthStencilView_t)HookVtable(imm, VT_D11CTX_ClearDepthStencilView, (void*)&Hk_ClearDepthStencilView);
        o_D11CopyResource = (D11CopyResource_t)HookVtable(imm, VT_D11CTX_CopyResource, (void*)&Hk_CopyResource);
        o_D11CopySubresourceRegion = (D11CopySubresourceRegion_t)HookVtable(imm, VT_D11CTX_CopySubresourceRegion, (void*)&Hk_CopySubresourceRegion);
        o_D11ResolveSubresource = (D11ResolveSubresource_t)HookVtable(imm, VT_D11CTX_ResolveSubresource, (void*)&Hk_ResolveSubresource);
        o_D11Dispatch = (D11Dispatch_t)HookVtable(imm, VT_D11CTX_Dispatch, (void*)&Hk_Dispatch);
        o_D11DispatchIndirect = (D11DispatchIndirect_t)HookVtable(imm, VT_D11CTX_DispatchIndirect, (void*)&Hk_DispatchIndirect);
        o_D11DrawAuto = (D11DrawAuto_t)HookVtable(imm, VT_D11CTX_DrawAuto, (void*)&Hk_DrawAuto);
        o_D11DrawInstancedIndirect = (D11DrawInstancedIndirect_t)HookVtable(imm, VT_D11CTX_DrawInstancedIndirect, (void*)&Hk_DrawInstancedIndirect);
        o_D11DrawIndexedInstancedIndirect = (D11DrawIndexedInstancedIndirect_t)HookVtable(imm, VT_D11CTX_DrawIndexedInstancedIndirect, (void*)&Hk_DrawIndexedInstancedIndirect);
        o_D11VSSetShaderResources = (D11VSSetShaderResources_t)HookVtable(imm, VT_D11CTX_VSSetShaderResources, (void*)&Hk_VSSetShaderResources);
        o_D11CSSetShaderResources = (D11CSSetShaderResources_t)HookVtable(imm, VT_D11CTX_CSSetShaderResources, (void*)&Hk_CSSetShaderResources);
        o_D11CSSetUnorderedAccessViews = (D11CSSetUnorderedAccessViews_t)HookVtable(imm, VT_D11CTX_CSSetUnorderedAccessViews, (void*)&Hk_CSSetUnorderedAccessViews);
        o_D11ExecuteCommandList = (D11ExecuteCommandList_t)HookVtable(imm, VT_D11CTX_ExecuteCommandList, (void*)&Hk_ExecuteCommandList);
        o_D11FinishCommandList = (D11FinishCommandList_t)HookVtable(imm, VT_D11CTX_FinishCommandList, (void*)&Hk_FinishCommandList);
        o_D11RSSetViewports = (D11RSSetViewports_t)HookVtable(imm, VT_D11CTX_RSSetViewports, (void*)&Hk_RSSetViewports);
        g_immVtbl = *(void**)imm;
#define CS(name) g_ctxSlots.push_back(CtxSlot{VT_D11CTX_##name, (void*)&Hk_##name, (void**)&o_D11##name, (*(void***)imm)[VT_D11CTX_##name]});
        CS(VSSetConstantBuffers) CS(HSSetConstantBuffers) CS(DSSetConstantBuffers) CS(GSSetConstantBuffers) CS(VSSetShader) CS(Draw) CS(DrawIndexed) CS(DrawInstanced) CS(DrawIndexedInstanced) CS(Map) CS(Unmap)
        CS(UpdateSubresource) CS(OMSetRenderTargets) CS(OMSetRenderTargetsAndUnorderedAccessViews) CS(PSSetShaderResources)
        CS(PSSetConstantBuffers) CS(PSSetShader) CS(ClearRenderTargetView) CS(ClearDepthStencilView) CS(CopyResource)
        CS(CopySubresourceRegion) CS(ResolveSubresource) CS(Dispatch) CS(DispatchIndirect) CS(DrawAuto) CS(DrawInstancedIndirect)
        CS(DrawIndexedInstancedIndirect) CS(VSSetShaderResources) CS(CSSetShaderResources) CS(CSSetUnorderedAccessViews)
        CS(ExecuteCommandList) CS(FinishCommandList) CS(RSSetViewports)
#undef CS
    }
#undef H
    // the swap chain may come from the device's own factory
    IDXGIDevice* dd = nullptr;
    if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dd)) && dd) {
        IDXGIAdapter* ad = nullptr;
        if (SUCCEEDED(dd->GetAdapter(&ad)) && ad) {
            DXGI_ADAPTER_DESC ad_{};
            ad->GetDesc(&ad_);
            LOGI("d3d11: adapter %S", ad_.Description);
            IDXGIFactory* f = nullptr;
            if (SUCCEEDED(ad->GetParent(__uuidof(IDXGIFactory), (void**)&f)) && f) { HookFactory(f); f->Release(); }
            ad->Release();
        }
        dd->Release();
    }
}

HRESULT WINAPI Hk_D3D11CreateDevice(IDXGIAdapter* a, D3D_DRIVER_TYPE t, HMODULE sw, UINT fl, const D3D_FEATURE_LEVEL* lv, UINT nlv, UINT sdk,
                                    ID3D11Device** dev, D3D_FEATURE_LEVEL* got, ID3D11DeviceContext** ctx) {
    ID3D11Device* d = nullptr; ID3D11DeviceContext* c = nullptr; D3D_FEATURE_LEVEL f = (D3D_FEATURE_LEVEL)0;
    HRESULT hr = o_D3D11CreateDevice(a, t, sw, fl, lv, nlv, sdk, &d, &f, &c);
    LOGI("d3d11: D3D11CreateDevice(driver %d, flags %x) -> %08lX", (int)t, fl, hr);
    if (SUCCEEDED(hr) && dev && d && t != D3D_DRIVER_TYPE_NULL && t != D3D_DRIVER_TYPE_REFERENCE) HookDevice(d, c, f);
    if (dev) *dev = d; else if (d) d->Release();
    if (ctx) *ctx = c; else if (c) c->Release();
    if (got) *got = f;
    return hr;
}
HRESULT WINAPI Hk_D3D11CreateDeviceAndSwapChain(IDXGIAdapter* a, D3D_DRIVER_TYPE t, HMODULE sw, UINT fl, const D3D_FEATURE_LEVEL* lv, UINT nlv,
                                                UINT sdk, const DXGI_SWAP_CHAIN_DESC* scd, IDXGISwapChain** sc, ID3D11Device** dev,
                                                D3D_FEATURE_LEVEL* got, ID3D11DeviceContext** ctx) {
    ID3D11Device* d = nullptr; ID3D11DeviceContext* c = nullptr; D3D_FEATURE_LEVEL f = (D3D_FEATURE_LEVEL)0;
    DXGI_SWAP_CHAIN_DESC dd{};
    if (scd && sc) { dd = *scd; DecideRenderSize(dd.BufferDesc.Width, dd.BufferDesc.Height); ForceSize(dd.BufferDesc.Width, dd.BufferDesc.Height, "D3D11CreateDeviceAndSwapChain"); scd = &dd; }
    HRESULT hr = o_D3D11CreateDeviceAndSwapChain(a, t, sw, fl, lv, nlv, sdk, scd, sc, &d, &f, &c);
    LOGI("d3d11: D3D11CreateDeviceAndSwapChain(driver %d, flags %x) -> %08lX", (int)t, fl, hr);
    if (SUCCEEDED(hr) && dev && d) HookDevice(d, c, f);
    if (SUCCEEDED(hr) && sc && *sc) HookSwapChain(*sc);
    if (dev) *dev = d; else if (d) d->Release();
    if (ctx) *ctx = c; else if (c) c->Release();
    if (got) *got = f;
    return hr;
}
HRESULT WINAPI Hk_CreateDXGIFactory(REFIID id, void** out) {
    HRESULT hr = o_CreateDXGIFactory(id, out);
    if (SUCCEEDED(hr) && out) HookFactory(*out);
    return hr;
}
HRESULT WINAPI Hk_CreateDXGIFactory1(REFIID id, void** out) {
    HRESULT hr = o_CreateDXGIFactory1(id, out);
    if (SUCCEEDED(hr) && out) HookFactory(*out);
    return hr;
}

} // namespace
void D3D11RenderSize(UINT gameW, UINT gameH, UINT* w, UINT* h) { D3D11RenderSizeImpl(gameW, gameH, w, h); }
bool D3D11ApplyRenderScale() { return ApplyRenderScaleImpl(); }
static volatile LONG g_scaleChangedTick = 0;
void D3D11RenderScaleChanged() { InterlockedExchange(&g_scaleChangedTick, (LONG)(GetTickCount() | 1)); }
void D3D11RenderScaleTick() {   // Present: apply once the slider has rested for 0.8 s
    LONG t = g_scaleChangedTick;
    if (t && GetTickCount() - (DWORD)t > 800 && InterlockedCompareExchange(&g_scaleChangedTick, 0, t) == t) ApplyRenderScaleImpl();
}
int GfFxCount() { return FxPassCount(); }
std::string GfFxSelect(int i) { return FxPassSelect(i); }

void D3D11LastFrame(bool* stereo, unsigned* patched) { *stereo = g_lastStereo11; *patched = g_lastPatched11; }

// Called once at start-up (DllMain is too early for LoadLibrary: from the first Direct3DCreate9 call or the init
// thread). Loads the system d3d11/dxgi (harmless for D3D9 games) and hooks their creation functions, so whichever
// API the game picks later, it is seen.
static void InstallOnce();
void InstallD3D11Hooks() {
    if (!G().s.d3d11Recon) return;
    static std::once_flag once;
    std::call_once(once, InstallOnce);   // the init thread and Direct3DCreate9 may both get here
}
static void InstallOnce() {
    MH_Initialize();
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    HMODULE d11 = LoadLibraryW((std::wstring(sys) + L"\\d3d11.dll").c_str());
    HMODULE dxgi = LoadLibraryW((std::wstring(sys) + L"\\dxgi.dll").c_str());
    auto hook = [](HMODULE m, const char* name, void* fn, void** orig) {
        void* p = m ? (void*)GetProcAddress(m, name) : nullptr;
        if (p && MH_CreateHook(p, fn, orig) == MH_OK && MH_EnableHook(p) == MH_OK) return true;
        LOGW("d3d11: could not hook %s", name);
        return false;
    };
    hook(d11, "D3D11CreateDevice", (void*)&Hk_D3D11CreateDevice, (void**)&o_D3D11CreateDevice);
    hook(d11, "D3D11CreateDeviceAndSwapChain", (void*)&Hk_D3D11CreateDeviceAndSwapChain, (void**)&o_D3D11CreateDeviceAndSwapChain);
    hook(dxgi, "CreateDXGIFactory", (void*)&Hk_CreateDXGIFactory, (void**)&o_CreateDXGIFactory);
    hook(dxgi, "CreateDXGIFactory1", (void*)&Hk_CreateDXGIFactory1, (void**)&o_CreateDXGIFactory1);
    LOGI("d3d11: recon hooks installed (D3D11CreateDevice*, CreateDXGIFactory*)");
}

} // namespace fvr

namespace fvr {
bool D3d11MainCameraPos(float out[3]) {
    if (!g_f.haveMain) return false;
    out[0] = g_f.mainCam.pos[0]; out[1] = g_f.mainCam.pos[1]; out[2] = g_f.mainCam.pos[2];
    return true;
}
void RequestCapture11() { g_captureReq = true; }
}
