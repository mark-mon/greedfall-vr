// Test aid (RE for head-aimed culling, see sharedtools\culling.txt): hardware read watchpoints on the gameplay render
// camera node - DR0 view*proj (+0x1A0), DR1 view (+0x160), DR2 world position row (+0x120), DR3 PCamera projection
// (+0x200) - on every thread for a few seconds. Each distinct reader instruction is logged once (Engine.dll offset,
// function start, code bytes, Engine.dll return addresses on the stack) and counted; counts logged at the end.
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <vector>
#include "log.h"
namespace fvr {
namespace {
PVOID g_veh = nullptr; HMODULE g_eng = nullptr;
struct Seen { uintptr_t at; int dr; volatile LONG n; };
Seen g_seen[96]; volatile LONG g_nSeen = 0; std::atomic<long> g_hits{0};
SRWLOCK g_lk = SRWLOCK_INIT;
const char* kWhat[4] = {"view*proj +0x1A0", "view +0x160", "world pos +0x120", "projection +0x200"};
uintptr_t EngOff(uintptr_t a) { return g_eng && a >= (uintptr_t)g_eng && a < (uintptr_t)g_eng + 0x2000000 ? a - (uintptr_t)g_eng : 0; }
LONG CALLBACK Veh(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    DWORD64 dr6 = ep->ContextRecord->Dr6; if (!(dr6 & 0xF)) return EXCEPTION_CONTINUE_SEARCH;
    ep->ContextRecord->Dr6 = 0;
    int dr = (dr6 & 1) ? 0 : (dr6 & 2) ? 1 : (dr6 & 4) ? 2 : 3;
    uintptr_t at = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;   // data breakpoints trap AFTER the instruction
    g_hits++;
    AcquireSRWLockExclusive(&g_lk);
    LONG n = g_nSeen; int i = 0;
    for (; i < n; ++i) if (g_seen[i].at == at && g_seen[i].dr == dr) { g_seen[i].n++; break; }
    bool isNew = i == n && n < 96;
    if (isNew) { g_seen[n].at = at; g_seen[n].dr = dr; g_seen[n].n = 1; g_nSeen = n + 1; }
    ReleaseSRWLockExclusive(&g_lk);
    if (isNew) {
        DWORD64 base = 0; PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry((DWORD64)at, &base, nullptr);
        char chain[400] = ""; int k = 0, found = 0; uintptr_t* sp = (uintptr_t*)ep->ContextRecord->Rsp;
        for (int s = 0; s < 160 && found < 8; ++s) { uintptr_t o = EngOff(sp[s]); if (o) { k += snprintf(chain + k, sizeof chain - k, " <- %llx", (unsigned long long)o); found++; } }
        char code[100] = ""; const uint8_t* c = (const uint8_t*)at - 12; for (int b = 0; b < 24; ++b) snprintf(code + b * 3, 4, "%02x ", c[b]);
        const CONTEXT& x = *ep->ContextRecord;
        LOGI("camwatch: NEW reader of %s at Engine.dll+%llx (function +%llx) thread %lu | code [-12..+12] %s| rax %llx rcx %llx rdx %llx r8 %llx r9 %llx |%s",
             kWhat[dr], (unsigned long long)EngOff(at), rf ? (unsigned long long)(rf->BeginAddress) : 0ull, GetCurrentThreadId(), code,
             x.Rax, x.Rcx, x.Rdx, x.R8, x.R9, chain);
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}
struct Args { DWORD tid; uintptr_t a[4]; bool set; };
DWORD WINAPI SetThr(void* p) {
    Args* a = (Args*)p;
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, a->tid); if (!th) return 1;
    SuspendThread(th);
    CONTEXT c{}; c.ContextFlags = CONTEXT_DEBUG_REGISTERS; GetThreadContext(th, &c);
    if (a->set) {
        c.Dr0 = a->a[0]; c.Dr1 = a->a[1]; c.Dr2 = a->a[2]; c.Dr3 = a->a[3];
        c.Dr7 = 0; for (int i = 0; i < 4; ++i) if (a->a[i]) c.Dr7 |= (1ull << (i * 2)) | (3ull << (16 + i * 4)) | (2ull << (18 + i * 4));   // R/W, 8 bytes
    } else { c.Dr0 = c.Dr1 = c.Dr2 = c.Dr3 = 0; c.Dr7 = 0; }
    SetThreadContext(th, &c); ResumeThread(th); CloseHandle(th); return 0;
}
uintptr_t g_addr[4];
void AllThreads(bool set) {
    std::vector<DWORD> t; HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); THREADENTRY32 te{sizeof te};
    if (snap != INVALID_HANDLE_VALUE) { for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) if (te.th32OwnerProcessID == GetCurrentProcessId() && te.th32ThreadID != GetCurrentThreadId()) t.push_back(te.th32ThreadID); CloseHandle(snap); }
    for (DWORD id : t) {
        Args a{id, {g_addr[0], g_addr[1], g_addr[2], g_addr[3]}, set};
        HANDLE h = CreateThread(nullptr, 0, SetThr, &a, 0, nullptr); if (h) { WaitForSingleObject(h, 2000); CloseHandle(h); }
    }
}
struct Run { uint8_t* node; int ms; };
DWORD WINAPI WatchThread(void* p) {
    Run r = *(Run*)p; delete (Run*)p;
    g_eng = GetModuleHandleW(L"Engine.dll");
    g_addr[0] = (uintptr_t)r.node + 0x1A0; g_addr[1] = (uintptr_t)r.node + 0x160; g_addr[2] = (uintptr_t)r.node + 0x120; g_addr[3] = (uintptr_t)r.node + 0x200;
    g_veh = AddVectoredExceptionHandler(1, Veh);
    AllThreads(true);
    LOGI("camwatch: read watch on camera node %p for %d ms (all threads)", (void*)r.node, r.ms);
    Sleep(r.ms);
    AllThreads(false);
    Sleep(200);
    RemoveVectoredExceptionHandler(g_veh); g_veh = nullptr;
    char line[4096]; int k = snprintf(line, sizeof line, "camwatch: %ld hits, counts:", g_hits.load());
    for (LONG i = 0; i < g_nSeen && k < (int)sizeof line - 80; ++i) k += snprintf(line + k, sizeof line - k, " [%s] %llx=%ld", kWhat[g_seen[i].dr] , (unsigned long long)EngOff(g_seen[i].at), g_seen[i].n);
    LOGI("%s", line);
    return 0;
}
}
void CamWatchStart(uint8_t* node, int ms) {
    static bool started = false; if (started || !node) return; started = true;
    HANDLE h = CreateThread(nullptr, 0, WatchThread, new Run{node, ms}, 0, nullptr); if (h) CloseHandle(h);
}
}
