// Crash reporting that also works when the crashing thread has no stack left (stack overflow).
// The first vectored handler does almost nothing on the faulting thread: it copies the exception record + CPU
// context into static memory and wakes a helper thread, which has its own stack. The helper logs the fault, the
// registers, a scan of the faulting thread's stack (return addresses in order + the most repeated ones = the loop
// of a runaway recursion) and writes a minidump. After a stack overflow the process is ended right there (any other
// handler would only fault again); other fatal faults continue to the normal handlers.
#include <windows.h>
#include <psapi.h>
#include <dbghelp.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include "log.h"
#include "globals.h"
#include "crashwatch.h"

namespace fvr {
namespace {

EXCEPTION_RECORD g_rec;
CONTEXT g_ctx;
DWORD g_tid = 0;
uintptr_t g_stackBase = 0, g_stackLimit = 0;
HANDLE g_evt = nullptr, g_done = nullptr;
volatile LONG g_once = 0;

struct Mod { uintptr_t lo, hi; std::string name; };
std::vector<Mod> Modules() {
    std::vector<Mod> out;
    HMODULE mods[1024]; DWORD need = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) return out;
    for (DWORD i = 0; i < need / sizeof(HMODULE) && i < 1024; ++i) {
        MODULEINFO mi{};
        char nm[MAX_PATH] = "?";
        GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof mi);
        GetModuleBaseNameA(GetCurrentProcess(), mods[i], nm, sizeof nm);
        if (_stricmp(nm, "dinput8.dll") == 0 && (void*)mods[i] == (void*)&__ImageBase) strcpy_s(nm, "GFVR(dinput8)");
        out.push_back({(uintptr_t)mi.lpBaseOfDll, (uintptr_t)mi.lpBaseOfDll + mi.SizeOfImage, nm});
    }
    return out;
}
std::string Name(const std::vector<Mod>& m, uintptr_t a) {
    char b[160];
    for (auto& x : m) if (a >= x.lo && a < x.hi) { snprintf(b, sizeof b, "%s+%llx", x.name.c_str(), (unsigned long long)(a - x.lo)); return b; }
    snprintf(b, sizeof b, "%llx", (unsigned long long)a);
    return b;
}
bool InCode(const std::vector<Mod>& m, uintptr_t a) {
    for (auto& x : m) if (a >= x.lo + 0x1000 && a < x.hi) return true;
    return false;
}

DWORD WINAPI Helper(LPVOID) {
    WaitForSingleObject(g_evt, INFINITE);
    auto mods = Modules();
    DWORD code = g_rec.ExceptionCode;
    const char* what = code == EXCEPTION_STACK_OVERFLOW ? "STACK OVERFLOW" : code == EXCEPTION_ACCESS_VIOLATION ? "ACCESS VIOLATION" : "FATAL EXCEPTION";
    LOGE("==== %s (%08lX) at %s, thread %lu, frame %ld", what, code, Name(mods, (uintptr_t)g_rec.ExceptionAddress).c_str(), g_tid, G().frame);
    if (code == EXCEPTION_ACCESS_VIOLATION && g_rec.NumberParameters >= 2)
        LOGE("  %s address %llx", g_rec.ExceptionInformation[0] == 0 ? "read of" : g_rec.ExceptionInformation[0] == 1 ? "write to" : "execute at",
             (unsigned long long)g_rec.ExceptionInformation[1]);
    LOGE("  rip %s  rsp %llx  rbp %llx", Name(mods, g_ctx.Rip).c_str(), (unsigned long long)g_ctx.Rsp, (unsigned long long)g_ctx.Rbp);
    LOGE("  rax %llx rbx %llx rcx %llx rdx %llx rsi %llx rdi %llx r8 %llx r9 %llx", (unsigned long long)g_ctx.Rax, (unsigned long long)g_ctx.Rbx,
         (unsigned long long)g_ctx.Rcx, (unsigned long long)g_ctx.Rdx, (unsigned long long)g_ctx.Rsi, (unsigned long long)g_ctx.Rdi,
         (unsigned long long)g_ctx.R8, (unsigned long long)g_ctx.R9);
    LOGE("  thread stack %llx..%llx (%llu KB used)", (unsigned long long)g_stackLimit, (unsigned long long)g_stackBase,
         (unsigned long long)((g_stackBase - g_ctx.Rsp) / 1024));
    // return-address candidates on the faulting thread's stack, nearest first
    std::map<uintptr_t, int> count;
    std::string first;
    int nFirst = 0;
    uintptr_t lo = g_ctx.Rsp & ~(uintptr_t)7, hi = g_stackBase;
    if (hi > lo + 2 * 1024 * 1024) hi = lo + 2 * 1024 * 1024;
    for (uintptr_t p = lo; p + 8 <= hi; p += 8) {
        MEMORY_BASIC_INFORMATION mb;
        uintptr_t v;
        __try { v = *(uintptr_t*)p; } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        (void)mb;
        if (v < 0x10000 || !InCode(mods, v)) continue;
        count[v]++;
        if (nFirst < 64) { first += Name(mods, v) + " "; nFirst++; }
    }
    LOGE("  stack (nearest first): %s", first.c_str());
    std::vector<std::pair<int, uintptr_t>> top;
    for (auto& kv : count) top.push_back({kv.second, kv.first});
    std::sort(top.rbegin(), top.rend());
    std::string rep;
    for (size_t i = 0; i < top.size() && i < 16; ++i) { char b[200]; snprintf(b, sizeof b, "%s x%d  ", Name(mods, top[i].second).c_str(), top[i].first); rep += b; }
    LOGE("  most frequent return addresses: %s", rep.c_str());
    LOGE("  modules: ");
    for (auto& x : mods) LOGE("    %llx-%llx %s", (unsigned long long)x.lo, (unsigned long long)x.hi, x.name.c_str());
    xrb::LogFlush();
    // minidump
    if (HMODULE dh = LoadLibraryW(L"dbghelp.dll")) {
        typedef BOOL(WINAPI* MDW)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION,
                                  PMINIDUMP_CALLBACK_INFORMATION);
        if (MDW w = (MDW)GetProcAddress(dh, "MiniDumpWriteDump")) {
            wchar_t path[MAX_PATH];
            swprintf_s(path, L"%scrash.dmp", G().runDir.c_str());
            HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                EXCEPTION_POINTERS ep{&g_rec, &g_ctx};
                MINIDUMP_EXCEPTION_INFORMATION mei{g_tid, &ep, FALSE};
                BOOL ok = w(GetCurrentProcess(), GetCurrentProcessId(), f, (MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory),
                            &mei, nullptr, nullptr);
                CloseHandle(f);
                LOGE("  crash dump %s: %S", ok ? "written" : "FAILED", path);
            }
        }
    }
    xrb::LogFlush();
    SetEvent(g_done);
    return 0;
}

bool Fatal(DWORD c) {
    return c == EXCEPTION_STACK_OVERFLOW || c == EXCEPTION_ACCESS_VIOLATION || c == EXCEPTION_ILLEGAL_INSTRUCTION || c == EXCEPTION_PRIV_INSTRUCTION ||
           c == EXCEPTION_INT_DIVIDE_BY_ZERO || c == 0xC0000409 || c == 0xC0000374;
}

// Runs on the faulting thread: keep the frame tiny (no strings, no logging).
LONG CALLBACK FirstHandler(PEXCEPTION_POINTERS ep) {
    DWORD c = ep->ExceptionRecord->ExceptionCode;
    if (c != EXCEPTION_STACK_OVERFLOW) return EXCEPTION_CONTINUE_SEARCH;   // others: the normal handler logs them
    if (InterlockedExchange(&g_once, 1) == 0) {
        g_rec = *ep->ExceptionRecord;
        g_ctx = *ep->ContextRecord;
        g_tid = GetCurrentThreadId();
        NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
        g_stackBase = (uintptr_t)tib->StackBase;
        g_stackLimit = (uintptr_t)tib->StackLimit;
        SetEvent(g_evt);
        WaitForSingleObject(g_done, 15000);
    } else WaitForSingleObject(g_done, 15000);
    TerminateProcess(GetCurrentProcess(), c);   // nothing else can run on this stack
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

std::string AddrName(const void* a) { return Name(Modules(), (uintptr_t)a); }

// Fatal faults other than stack overflow: the normal handler calls this so the same detailed report is written
void CrashWatchReport(PEXCEPTION_POINTERS ep) {
    if (!g_evt || !Fatal(ep->ExceptionRecord->ExceptionCode)) return;
    if (InterlockedExchange(&g_once, 1) != 0) return;
    g_rec = *ep->ExceptionRecord;
    g_ctx = *ep->ContextRecord;
    g_tid = GetCurrentThreadId();
    NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
    g_stackBase = (uintptr_t)tib->StackBase;
    g_stackLimit = (uintptr_t)tib->StackLimit;
    SetEvent(g_evt);
    WaitForSingleObject(g_done, 15000);
}

void InstallCrashWatch() {
    if (g_evt) return;
    g_evt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (HANDLE t = CreateThread(nullptr, 256 * 1024, Helper, nullptr, 0, nullptr)) CloseHandle(t);
    AddVectoredExceptionHandler(1, FirstHandler);   // first in the chain
    LOGI("crash watch installed (stack overflow safe)");
}

// ---- loaded modules: all at start, then every new one (overlays, injectors, input tools)
static DWORD WINAPI ModuleWatch(LPVOID) {
    std::map<uintptr_t, bool> seen;
    for (int round = 0;; ++round) {
        auto mods = Modules();
        int added = 0;
        for (auto& m : mods) {
            if (seen.count(m.lo)) continue;
            seen[m.lo] = true;
            char path[MAX_PATH] = "";
            GetModuleFileNameA((HMODULE)m.lo, path, sizeof path);
            if (round == 0) LOGI("module: %s  %llx (%llu KB)  %s", m.name.c_str(), (unsigned long long)m.lo, (unsigned long long)((m.hi - m.lo) / 1024), path);
            else LOGI("module loaded: %s  %llx  %s", m.name.c_str(), (unsigned long long)m.lo, path);
            added++;
        }
        if (round == 0) LOGI("module: %d modules at start", added);
        Sleep(round < 30 ? 1000 : 5000);
    }
    return 0;
}
void StartModuleWatch() {
    if (HANDLE t = CreateThread(nullptr, 256 * 1024, ModuleWatch, nullptr, 0, nullptr)) CloseHandle(t);
}

} // namespace fvr
