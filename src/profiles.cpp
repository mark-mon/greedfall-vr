// UEVR3 per-game profiles (see profiles.h). Add a line per tested game.
#include "profiles.h"
#include <cwchar>

namespace fvr {

// Generic: culling-FOV widening OFF. Its search sets hardware data breakpoints on the game's threads, and some games end
// the process silently when they see them (anti-debug: XCOM EU, Injustice - smoke tests 058/059/072/073). Profiles of
// games where the search was tested turn it on.
static const Profile kGeneric = {L"", "Unreal Engine 3 game", 50.f, 1.0f, 12, "generic UE3 defaults (cull_fov off)"};

static const Profile kProfiles[] = {
    // exe              name                  units/m cull  min draws
    {L"greedfall.exe", "GreedFall", 100.f, 4.0f, 12,
     "GreedFall VR (gfvr): Silk engine, scene recorded on 16 deferred contexts; cb0 = View/Proj/VP/camera, RH, 1 unit = 1 cm; UI texture -> HUD layer"},
    {L"tlr.exe", "The Last Remnant", 100.f, 1.5f, 12,
     "tested 2026-10-31: stereo, HUD layer (minimap UI scene), cull FOV found (45 deg camera), 1 unit = 1 cm; title backdrop camera 20 deg", 30.f},
    // smoke tests 2026-11-01 (menus / intro only, no saves): stereo + HUD layer work with the generic rules
    {L"dishonored.exe", "Dishonored", 50.f, 1.0f, 12, "user 2026-11-06: works basically perfectly"},
    {L"rememberme.exe", "Remember Me", 50.f, 1.5f, 12,
     "smoke 057: stereo, cull FOV found (75 deg). user 2026-11-02 at 3840x2160: tiny DXT mips failed to upload (fixed), then a crash in the NVIDIA driver before the menu (open; try 1920x1080)"},
    {L"apgame.exe", "Alpha Protocol", 50.f, 1.5f, 60,
     "user 2026-11-02: works in game; title menu head-locked (UI scene in the 3D scene) -> ui_in_scene_flat; objects vanish at the sides -> cull search on", 0.f,
     "ui_in_scene_flat=1"},
    {L"fable anniversary.exe", "Fable Anniversary", 50.f, 1.0f, 12,
     "user 2026-11-02: HUD layer good; letters and body parts missing -> keep_texture_copies (fixed 2026-11-02)"},
    {L"riseoftheargonauts.exe", "Rise of the Argonauts", 50.f, 1.0f, 12, "smoke 049: stereo in the intro"},
    {L"styxgame.exe", "Styx: Master of Shadows", 50.f, 1.0f, 12, "smoke 050: stereo, UI scenes on HUD; cull search not found"},
    {L"astronautsgame-win32-shipping.exe", "The Vanishing of Ethan Carter", 50.f, 1.5f, 12, "smoke 051: stereo, cull FOV found (97 deg)"},
    {L"injustice.exe", "Injustice: Gods Among Us", 50.f, 1.0f, 12, "smoke 068/069: stereo; dies silently with the cull search + HUD layer - keep cull_fov 1"},
    {L"xcomgame.exe", "XCOM: Enemy Unknown", 50.f, 1.0f, 12, "smoke 070/071: stereo; the cull search ends the game (anti-debug?) - keep cull_fov 1"},
    {L"batmanorigins.exe", "Batman: Arkham Origins", 50.f, 1.0f, 12, "smoke 067: needs AllowD3D11=False in BmEngine.ini; stereo menu backdrop"},
    {L"lifeisstrange.exe", "Life Is Strange", 50.f, 1.5f, 12, "smoke 055: stereo, UI scenes on HUD, cull FOV found (68 deg)"},
    // user tests 2026-11-02 (real headset, RTX 5090)
    {L"shippingpc-bzb2game.exe", "All Zombies Must Die!", 50.f, 1.0f, 12, "user 2026-11-06: works perfectly (faces fixed by keep_texture_copies, L3+R3 menu works)"},
    {L"shippingpc-bmgame.exe", "Batman: Arkham Asylum", 50.f, 1.0f, 12, "user: seems to work well"},
    {L"moha.exe", "Medal of Honor: Airborne", 50.f, 1.0f, 12, "user: freezes at the intro movie (game at ~2900 fps, 2 draws: movie); open"},
    {L"thebureau.exe", "The Bureau: XCOM Declassified", 50.f, 1.0f, 12, "user: exits at once (frame 0, before D3D) from the 2K launcher's DX9 choice; open"},
    {L"dndgame.exe", "Dungeons & Dragons: Daggerdale", 50.f, 1.0f, 12, "smoke 065: start through Steam (direct start exits); stereo"},
};

const Profile& DetectProfile(const wchar_t* exe) {
    for (const Profile& p : kProfiles)
        if (!wcscmp(p.exe, exe)) return p;
    return kGeneric;
}

} // namespace fvr
