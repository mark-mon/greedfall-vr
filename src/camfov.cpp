// Culling FOV widening - see camfov.h and claude/culling-fov.md.
//
// Data only, never code. What the search found (2026-10-21): the game camera keeps its vertical field of view as a
// float in radians (0.6545 = 37.5 deg in the field) in a heap object, and recomputes it twice per frame; the
// projection (and the culling frustum) are derived from it. Writing it once has no effect, so:
//  1. A background thread scans the game's writable heap for floats equal to the current vertical FOV in radians
//     (derived from the projection the game sends to its shaders).
//  2. Each candidate gets a CPU data breakpoint (debug registers, "break on write") for 20 frames: right after each
//     game write the handler replaces the value with a 30% wider one. The candidate is accepted when both P00 and
//     P11 of the game's projection shrink by that factor.
//  3. The found value is then kept widened by `cull_fov` the same way. If it stops being written (the camera object
//     was replaced, e.g. a battle), the search runs again.
#include "camfov.h"
#include "stereo.h"
#include "globals.h"
#include <psapi.h>
#include <tlhelp32.h>
#include <vector>
#include <cmath>

namespace fvr {

static const float kTestScale = 1.3f;
static const long kTestFrames = 20, kSettleFrames = 5, kRefreshFrames = 300, kLostFrames = 120;

// Wider vertical FOV (radians, full angle) for a frustum `scale` times as wide (tangent space).
static float Widen(float fov, double scale) { return (float)(2 * atan(tan(fov / 2.0) * scale)); }
// The same for a value in degrees (Lightning Returns keeps its FOV in degrees; FF XIII and XIII-2 in radians).
static float WidenDeg(float fov, double scale) { return (float)(Widen((float)(fov * 3.14159265358979 / 180), scale) * 180 / 3.14159265358979); }

// ------------------------------------------------------------------ write traps (CPU data breakpoints DR0-DR3)
// Up to 4 values are trapped at once (the counting pass tests 4 candidates per round). Setting the debug registers
// means suspending every thread of the game once; that runs on a helper thread and the state machine waits for it
// without blocking the render thread (it used to wait up to 2 s per candidate on the render thread: the slide show
// in the first minutes of The Last Remnant in the headset, 2026-11-01).
struct Slot {
    float* volatile addr = nullptr;
    volatile double scale = 1;
    float written = 0;
    volatile bool deg = false;   // the value is in degrees
    volatile LONG hits = 0;
};
static Slot g_slot[4];
static PVOID g_veh = nullptr;
static HANDLE g_setter = nullptr;   // helper thread applying the slots to all threads

static LONG CALLBACK BpHandler(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    DWORD fired = ep->ContextRecord->Dr6 & 0xF;
    if (!fired) return EXCEPTION_CONTINUE_SEARCH;
    ep->ContextRecord->Dr6 = 0;
    for (int i = 0; i < 4; ++i) {
        if (!(fired & (1u << i))) continue;
        Slot& s = g_slot[i];
        float* a = s.addr;
        if (!a) continue;
        InterlockedIncrement(&s.hits);
        float v = *a;
        double sc = s.scale;
        // our own write traps too; and only plausible FOV values are touched (5..120 degrees)
        bool deg = s.deg;
        if (v != s.written && sc != 1 && (deg ? (v > 5.f && v < 120.f) : (v > 0.087f && v < 2.1f))) {
            float w = deg ? WidenDeg(v, sc) : Widen(v, sc);
            s.written = w;
            *a = w;
        }
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

// DR0-DR3/DR7 on every thread of the process (from a helper thread, so the render thread is included).
static DWORD WINAPI BpSetter(LPVOID) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    DWORD_PTR dr[4], dr7 = 0;
    for (int i = 0; i < 4; ++i) {
        dr[i] = (DWORD_PTR)g_slot[i].addr;
        if (dr[i]) dr7 |= (1u << (2 * i)) | (1u << (16 + 4 * i)) | (3u << (18 + 4 * i));   // local enable, write, 4 bytes
    }
    THREADENTRY32 te{sizeof(te)};
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
        HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!th) continue;
        if (SuspendThread(th) != (DWORD)-1) {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(th, &ctx)) {
                ctx.Dr0 = dr[0]; ctx.Dr1 = dr[1]; ctx.Dr2 = dr[2]; ctx.Dr3 = dr[3];
                ctx.Dr7 = (ctx.Dr7 & 0x0000FF00u) | dr7;
                ctx.Dr6 = 0;
                SetThreadContext(th, &ctx);
            }
            ResumeThread(th);
        }
        CloseHandle(th);
    }
    CloseHandle(snap);
    return 0;
}

// true while the helper thread is still applying the last change
static bool TrapsBusy() {
    if (!g_setter) return false;
    if (WaitForSingleObject(g_setter, 0) == WAIT_TIMEOUT) return true;
    CloseHandle(g_setter);
    g_setter = nullptr;
    return false;
}
static void ApplyTraps() {
    if (g_setter) { WaitForSingleObject(g_setter, 2000); CloseHandle(g_setter); g_setter = nullptr; }
    g_setter = CreateThread(nullptr, 0, BpSetter, nullptr, 0, nullptr);
}
static void SetSlot(int i, float* addr, double scale, bool deg) {
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, BpHandler);
    Slot& s = g_slot[i];
    s.deg = deg; s.scale = scale; s.written = 0; s.hits = 0; s.addr = addr;
}
static void SetTrap(float* addr, double scale, bool deg) {   // one value (slot 0), the others off
    for (int i = 1; i < 4; ++i) g_slot[i].addr = nullptr;
    SetSlot(0, addr, scale, deg);
    ApplyTraps();
}
static void ClearTrap() {
    bool any = false;
    for (auto& s : g_slot) { any |= s.addr != nullptr; s.addr = nullptr; }
    if (any) ApplyTraps();
}

// ------------------------------------------------------------------ background memory scan
static const size_t kMaxCands = 16384;   // common values (65.0 degrees: Alpha Protocol) occur thousands of times
static float* g_scanOut[kMaxCands];
static bool g_scanDeg[kMaxCands];        // candidate holds degrees (else radians)
static volatile LONG g_scanCount = 0, g_scanDone = 0;
static float g_scanTarget = 0;           // radians; the degree form is searched too
static size_t g_scanBytes = 0;

static size_t ScanRegion(const char* base, size_t size, float t, float tDeg, size_t count) {
    __try {
        const float* p = (const float*)base;
        for (size_t i = 0; i < size / 4 && count < kMaxCands; ++i)
            if (fabsf(p[i] - t) <= t * 2e-5f) { g_scanDeg[count] = false; g_scanOut[count++] = (float*)&p[i]; }
            else if (fabsf(p[i] - tDeg) <= tDeg * 2e-5f) { g_scanDeg[count] = true; g_scanOut[count++] = (float*)&p[i]; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return count;
}

// Thread stacks: locals there change meaning all the time, never trap them (a private allocation with a guard page).
static bool IsStack(const MEMORY_BASIC_INFORMATION& m) {
    MEMORY_BASIC_INFORMATION r;
    for (const char* q = (const char*)m.AllocationBase; VirtualQuery(q, &r, sizeof(r)) && r.AllocationBase == m.AllocationBase;
         q = (const char*)r.BaseAddress + r.RegionSize)
        if (r.Protect & PAGE_GUARD) return true;
    return false;
}

static DWORD WINAPI ScanThread(LPVOID) {
    t_memProbe = true;   // memory can be freed while we read it: not a crash (ScanRegion catches it)
    HMODULE exe = GetModuleHandleW(nullptr);
    size_t count = 0, bytes = 0;
    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t a = 0x10000; a < 0x7FFF0000 && VirtualQuery((void*)a, &mbi, sizeof(mbi)); a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize) {
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) continue;
        if (!(mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY))) continue;          // writable data only
        if (mbi.Type == MEM_IMAGE && mbi.AllocationBase != (void*)exe) continue;   // DLL data (incl. ours)
        if (mbi.Type == MEM_PRIVATE && IsStack(mbi)) continue;
        if (mbi.Type == MEM_MAPPED) continue;                                      // shared / driver-mapped sections
        if (mbi.Protect & (PAGE_WRITECOMBINE | PAGE_NOCACHE)) continue;            // GPU upload memory (the driver reads it)
        bytes += mbi.RegionSize;
        count = ScanRegion((const char*)mbi.BaseAddress, mbi.RegionSize, g_scanTarget, g_scanTarget * 57.2957795f, count);
    }
    g_scanBytes = bytes;
    g_scanCount = (LONG)count;
    InterlockedExchange(&g_scanDone, 1);
    return 0;
}

// ------------------------------------------------------------------ write filter (many candidates)
// Each candidate still holding the value gets nudged by one float step; a few frames later the ones the game wrote
// back to the exact value are its live camera variables (UE3 rewrites FOVAngle every frame). Everything else is put
// back as it was. One-ulp changes for 4 frames, no breakpoints: thousands of candidates in one go.
static std::vector<float> g_pertOrig;
static std::vector<char> g_pertState;   // 0 not touched, 1 nudged
static void PerturbAll(const std::vector<float*>& c) {
    g_pertOrig.assign(c.size(), 0.f);
    g_pertState.assign(c.size(), 0);
    for (size_t i = 0; i < c.size(); ++i) {
        __try {
            float v = *c[i];
            g_pertOrig[i] = v;
            *c[i] = nextafterf(v, 3.4e38f);
            g_pertState[i] = 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}
// rewritten candidates -> keep; nudged values still there are restored
static void CheckPerturbed(const std::vector<float*>& c, const std::vector<bool>& deg, std::vector<float*>* keep, std::vector<bool>* keepDeg,
                           size_t* restored) {
    for (size_t i = 0; i < c.size(); ++i) {
        if (!g_pertState[i]) continue;
        __try {
            float v = *c[i], o = g_pertOrig[i];
            if (v == o) { keep->push_back(c[i]); keepDeg->push_back(deg[i]); }
            else if (v == nextafterf(o, 3.4e38f)) { *c[i] = o; (*restored)++; }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}
static const size_t kPerturbAbove = 32;
// More candidates than this: the value is too common to nudge safely (Fable: a 10 degree zoom camera, 16384 places,
// followed by a GPU hang - some of those were driver data). The search gives up for this FOV instead.
static const size_t kPerturbMax = 2048;
static const long kPerturbFrames = 4;

// ------------------------------------------------------------------ state machine (render thread, once per frame)
enum State { S_IDLE, S_SCANNING, S_PERTURB_WAIT, S_COUNT, S_COUNT_WAIT, S_TRY, S_TRY_WAIT, S_SETTLE, S_ACTIVE };
static const long kCountFrames = 8;
static State g_state = S_IDLE;
static long g_until = 0, g_nextSearch = 0;
static std::vector<float*> g_cands;
static std::vector<bool> g_candDeg;
static std::vector<float*> g_keep;   // candidates the game writes every frame (counting pass)
static std::vector<bool> g_keepDeg;
static bool g_foundDeg = false;
static size_t g_next = 0, g_batch = 0;
static float g_baseP00 = 0, g_baseP11 = 0;
static float* g_found = nullptr;
static double g_scale = 0;           // 0 = from config (cull_fov)
static LONG g_lastHits = 0;
static long g_lastHitFrame = 0, g_refreshAt = 0;
static unsigned g_searches = 0, g_failed = 0;   // searches stop after kMaxFailed misses in a row (each one costs frame rate)
static const unsigned kMaxFailed = 3;
static float g_failTarget = 0;   // the FOV the misses were for: another camera FOV (new scene) gets new attempts

static long Frame() { return G().frame; }
static double Scale() { return g_scale > 0 ? g_scale : G().s.cullFov; }

// The game's projection of the latest frame, if it is recent (we're in the 3D scene).
static bool Projection(float* p00, float* p11) {
    long f;
    return StereoGameProjection(p00, p11, &f) && Frame() - f <= 2;
}

static std::string Where(const void* p) {
    HMODULE exe = GetModuleHandleW(nullptr);
    MODULEINFO mi{};
    GetModuleInformation(GetCurrentProcess(), exe, &mi, sizeof(mi));
    char b[64];
    uintptr_t a = (uintptr_t)p, base = (uintptr_t)mi.lpBaseOfDll;
    if (a >= base && a < base + mi.SizeOfImage) sprintf_s(b, "exe+%Ix", a - base);
    else sprintf_s(b, "heap %p", p);
    return b;
}

// UE3 keeps the camera's horizontal FOV in degrees (FOVAngle): search that (The Last Remnant: 45).
static float SearchTarget(float p00, float p11) {
    return G().game == GAME_UE3 ? (float)(2 * atan(1.0 / p00)) : (float)(2 * atan(1.0 / p11));
}
static void StartSearch(float p00, float p11) {
    g_scanTarget = SearchTarget(p00, p11);
    g_scanDone = 0;
    g_scanCount = 0;
    HANDLE t = CreateThread(nullptr, 0, ScanThread, nullptr, 0, nullptr);
    if (!t) { g_nextSearch = Frame() + 600; return; }
    SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL);
    CloseHandle(t);
    g_searches++;
    g_state = S_SCANNING;
}

void CamFovTick() {
    double scale = Scale();
    float p00, p11;
    switch (g_state) {
    case S_IDLE: {
        if (scale <= 1.001 || !Projection(&p00, &p11)) return;
        float target = SearchTarget(p00, p11);
        float tdeg = target * 57.2958f;
        if (tdeg < 30.f || tdeg > 130.f) return;   // zoom / cutscene / special cameras: no search (kept for gameplay FOVs)
        if (g_failed && fabsf(target / g_failTarget - 1) > 0.01f) { g_failed = 0; g_nextSearch = 0; }   // a different camera FOV
        if (Frame() < g_nextSearch) return;
        StartSearch(p00, p11);
    }
        return;
    case S_SCANNING:
        if (!g_scanDone) return;
        g_cands.assign(g_scanOut, g_scanOut + g_scanCount);
        g_candDeg.assign(g_scanDeg, g_scanDeg + g_scanCount);
        g_next = 0;
        LOGI("fov: search %u: %u candidates for %.2f deg (camera FOV, radians or degrees) in %.0f MB", g_searches, (unsigned)g_cands.size(),
             g_scanTarget * 57.2958f, g_scanBytes / 1048576.0);
        g_keep.clear(); g_keepDeg.clear();
        if (g_cands.size() > kPerturbMax) {
            LOGW("fov: %u places hold this value - too common to test safely; no culling widening for this camera FOV", (unsigned)g_cands.size());
            g_failTarget = g_scanTarget;
            g_failed = kMaxFailed;
            g_nextSearch = 0x7FFFFFFF;
            g_state = S_IDLE;
            return;
        }
        if (g_cands.size() > kPerturbAbove) {
            PerturbAll(g_cands);
            g_until = Frame() + kPerturbFrames;
            g_state = S_PERTURB_WAIT;
            return;
        }
        g_state = S_COUNT;
        return;
    case S_PERTURB_WAIT: {
        if (Frame() < g_until) return;
        std::vector<float*> k;
        std::vector<bool> kd;
        size_t restored = 0;
        CheckPerturbed(g_cands, g_candDeg, &k, &kd, &restored);
        LOGI("fov: write filter: %u of %u candidates rewritten by the game within %ld frames (%u restored)", (unsigned)k.size(),
             (unsigned)g_cands.size(), kPerturbFrames, (unsigned)restored);
        if (k.size() > 256) k.resize(256), kd.resize(256);
        g_cands = k; g_candDeg = kd;
        g_next = 0;
        g_state = S_COUNT;
        return;
    }
    // Counting pass: a breakpoint that only counts writes (scale 1 = never modifies). The camera writes its FOV a few
    // times per frame; constants and unrelated data with the same value (42 degrees is a common number) are not
    // rewritten and are never touched by the widening test below (an earlier version corrupted d3d9 data that way).
    case S_COUNT:
        if (g_next >= g_cands.size()) {
            LOGI("fov: %u of %u candidates are written every frame", (unsigned)g_keep.size(), (unsigned)g_cands.size());
            g_cands = g_keep; g_candDeg = g_keepDeg;
            g_next = 0;
            g_state = S_TRY;
            return;
        }
        if (!Projection(&p00, &p11)) return;
        for (int i = 0; i < 4; ++i) g_slot[i].addr = nullptr;
        g_batch = 0;
        while (g_batch < 4 && g_next + g_batch < g_cands.size()) {
            SetSlot((int)g_batch, g_cands[g_next + g_batch], 1.0, g_candDeg[g_next + g_batch]);
            g_batch++;
        }
        ApplyTraps();
        g_until = Frame() + kCountFrames;
        g_state = S_COUNT_WAIT;
        return;
    case S_COUNT_WAIT: {
        if (TrapsBusy()) { g_until = Frame() + kCountFrames; return; }   // count from when the traps are live
        if (Frame() < g_until) return;
        for (size_t i = 0; i < g_batch; ++i) {
            double perFrame = g_slot[i].hits / (double)kCountFrames;
            if (perFrame >= 0.5 && perFrame <= 12) { g_keep.push_back(g_cands[g_next + i]); g_keepDeg.push_back(g_candDeg[g_next + i]); }
        }
        ClearTrap();
        g_next += g_batch;
        g_state = S_COUNT;
        return;
    }
    case S_TRY:
        if (g_next >= g_cands.size()) {
            g_failTarget = g_scanTarget;
            if (++g_failed >= kMaxFailed) {
                LOGW("fov: the camera FOV value was not found (%u searches); culling FOV stays off until the camera FOV changes", g_failed);
                g_nextSearch = 0x7FFFFFFF;
            } else {
                LOGW("fov: the camera FOV value was not found; trying again in 10 s");
                g_nextSearch = Frame() + 600;
            }
            g_state = S_IDLE;
            return;
        }
        if (!Projection(&p00, &p11)) return;   // menu/movie: wait
        if (TrapsBusy()) return;
        g_baseP00 = p00; g_baseP11 = p11;
        SetTrap(g_cands[g_next], kTestScale, g_candDeg[g_next]);
        g_until = Frame() + kTestFrames;
        g_state = S_TRY_WAIT;
        return;
    case S_TRY_WAIT: {
        if (TrapsBusy()) { g_until = Frame() + kTestFrames; return; }
        if (Frame() < g_until) return;
        bool have = Projection(&p00, &p11);
        LONG hits = g_slot[0].hits;
        ClearTrap();
        bool cdeg = g_candDeg[g_next];
        float* c = g_cands[g_next++];
        // accepted when both axes shrink by the test factor (in tangent space: P = 1 / tan(half angle))
        bool ok = have && hits > 0 && fabsf(g_baseP11 / p11 / kTestScale - 1) < 0.03f && fabsf(g_baseP00 / p00 / kTestScale - 1) < 0.03f;
        if (ok) {
            g_found = c;
            g_foundDeg = cdeg;
            g_failed = 0;
            LOGI("fov: camera FOV found at %s (%.1f writes/frame): projection P00 %.4f -> %.4f, P11 %.4f -> %.4f with a %.0f%% wider value",
                 Where(c).c_str(), hits / (double)kTestFrames, g_baseP00, p00, g_baseP11, p11, (kTestScale - 1) * 100);
            SetTrap(g_found, scale, g_foundDeg);
            LOGI("fov: culling FOV x%.2f", scale);
            g_lastHits = 0;
            g_lastHitFrame = Frame();
            g_refreshAt = Frame() + kRefreshFrames;
            g_state = S_ACTIVE;
            return;
        }
        g_until = Frame() + kSettleFrames;
        g_state = S_SETTLE;
        return;
    }
    case S_SETTLE:
        if (Frame() >= g_until) g_state = S_TRY;
        return;
    case S_ACTIVE: {
        if (scale <= 1.001) { ClearTrap(); g_found = nullptr; g_state = S_IDLE; LOGI("fov: culling FOV back to the game's"); return; }
        if (g_slot[0].scale != scale) { g_slot[0].scale = scale; LOGI("fov: culling FOV x%.2f", scale); }
        LONG h = g_slot[0].hits;
        if (h != g_lastHits) { g_lastHits = h; g_lastHitFrame = Frame(); }
        else if (Frame() - g_lastHitFrame > kLostFrames && Projection(&p00, &p11)) {
            // 3D scene, but our value isn't written any more: the camera object was replaced. Search again.
            LOGI("fov: camera FOV value at %s no longer used; searching again", Where(g_found).c_str());
            ClearTrap();
            g_found = nullptr;
            g_state = S_IDLE;
            g_nextSearch = Frame() + 30;
            return;
        }
        if (Frame() >= g_refreshAt) {   // threads created since then get the breakpoint too
            g_refreshAt = Frame() + kRefreshFrames;
            if (!TrapsBusy()) {
                SetTrap(g_found, scale, g_foundDeg);
                g_lastHits = 0;
                g_lastHitFrame = Frame();
            }
        }
        return;
    }
    }
}

void CamFovScan() {
    if (g_state == S_ACTIVE) ClearTrap();
    g_found = nullptr;
    if (g_scale <= 0 && G().s.cullFov <= 1.001) g_scale = 1.5;   // a search needs a scale to apply afterwards
    g_state = S_IDLE;
    g_nextSearch = 0;
}

void CamFovSet(float scale) {
    g_scale = scale < 1 ? 1 : scale;
    LOGI("fov: culling FOV set to x%.2f by script", g_scale);
}

void CamFovStatus(char* buf, size_t n) {
    switch (g_state) {
    case S_IDLE:
        if (Scale() <= 1.001) sprintf_s(buf, n, "off (culling FOV x1)");
        else if (g_nextSearch == 0x7FFFFFFF) sprintf_s(buf, n, "not found (gave up after %u searches)", g_failed);
        else sprintf_s(buf, n, "waiting for a 3D scene");
        break;
    case S_SCANNING: sprintf_s(buf, n, "searching: scanning memory"); break;
    case S_PERTURB_WAIT: sprintf_s(buf, n, "searching: filtering %u values", (unsigned)g_cands.size()); break;
    case S_COUNT: case S_COUNT_WAIT:
        sprintf_s(buf, n, "searching: checking %u / %u values (slower frames)", (unsigned)g_next, (unsigned)g_cands.size()); break;
    case S_TRY: case S_TRY_WAIT: case S_SETTLE:
        sprintf_s(buf, n, "searching: testing %u / %u (slower frames)", (unsigned)g_next, (unsigned)g_cands.size()); break;
    case S_ACTIVE: sprintf_s(buf, n, "active x%.2f (%s)", Scale(), Where(g_found).c_str()); break;
    }
}

void CamFovLogStatus() {
    if (g_state == S_ACTIVE)
        LOGI("fov: culling FOV x%.2f active (%s)", Scale(), Where(g_found).c_str());
}

} // namespace fvr
