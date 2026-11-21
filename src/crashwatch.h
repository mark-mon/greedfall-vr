#pragma once
#include <windows.h>
extern "C" IMAGE_DOS_HEADER __ImageBase;
namespace fvr {
void InstallCrashWatch();                       // first vectored handler + helper thread (stack-overflow safe report)
void CrashWatchReport(PEXCEPTION_POINTERS ep);  // detailed report for other fatal faults (called by the normal handler)
void StartModuleWatch();
}
#include <string>
namespace fvr {
std::string AddrName(const void* a);           // module+offset (slow: enumerates modules - diagnostics only)                        // log all loaded modules, then each newly loaded one
}
