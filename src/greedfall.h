#pragma once
namespace fvr {
void InstallGreedfallFixes();     // from DllMain: waits for Engine.dll / Game.dll in a thread, then hooks
void GreedfallTick();             // once per frame (Present): keeps the camera-shake switch in line with the setting
const char* GreedfallStatus();    // one line for the periodic log
}
