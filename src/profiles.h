#pragma once
// UEVR3 per-game profiles. The core is generic for Unreal Engine 3 / D3D9; a profile only changes defaults
// (and later: switches for per-game fixes). Unknown games run with the generic UE3 defaults.
namespace fvr {

struct Profile {
    const wchar_t* exe;        // lower-case exe file name
    const char* name;          // display name (menu title, logs)
    float worldScale;          // game units per metre (Epic's UE3 convention: 1 unit ~ 2 cm -> 50)
    float cullFov;             // culling widening factor (camfov.cpp); 1 = off
    int stereoMinDraws;        // 3D draws a frame needs to be shown in stereo
    const char* notes;         // what is known / tested
    float stereoMinFov = 0;    // cameras narrower than this (horizontal degrees) are never 3D scenes (title backdrops)
    const char* tweaks = "";   // "key=value key=value": config defaults for this game (Tweaks page), the user's file wins
};

const Profile& DetectProfile(const wchar_t* exeLower);   // never null: the generic profile for unknown games

} // namespace fvr
