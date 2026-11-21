#include "vrkit_input.h"
#include "vrkit_options.h"
#include <windows.h>
#include <xinput.h>
#include "MinHook.h"
#include "log.h"

namespace vrkit { namespace input {

namespace {
Settings g_set;
using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
struct Hooked { const wchar_t* dll; void* target; GetStateFn orig; GetStateFn origEx; void* targetEx; };
Hooked g_mods[3] = { { L"xinput1_4.dll" }, { L"xinput1_3.dll" }, { L"xinput9_1_0.dll" } };
GetStateFn g_read = nullptr;                // an original XInputGetState for our own polling
volatile bool g_block = false, g_chordHeld = false;
DWORD g_fakePacket[4] = {};

volatile bool g_untilRelease = false;   // after closing: keep blocking until the pad is released
DWORD Filter(DWORD r, DWORD user, XINPUT_STATE* st) {
    if (r == ERROR_SUCCESS && st && g_untilRelease && !g_block) {
        const XINPUT_GAMEPAD& p = st->Gamepad;
        if (!p.wButtons && p.bLeftTrigger < 30 && p.bRightTrigger < 30) g_untilRelease = false;
    }
    // while all chord buttons are down together, the game doesn't see them (no stray L3 / R3 actions)
    if (r == ERROR_SUCCESS && st && g_set.chordButtons && (st->Gamepad.wButtons & g_set.chordButtons) == g_set.chordButtons)
        st->Gamepad.wButtons &= ~g_set.chordButtons;
    if (r == ERROR_SUCCESS && st && g_set.chordButtonsAlt && (st->Gamepad.wButtons & g_set.chordButtonsAlt) == g_set.chordButtonsAlt)
        st->Gamepad.wButtons &= ~g_set.chordButtonsAlt;
    if (r == ERROR_SUCCESS && st && (g_block || g_chordHeld || g_untilRelease)) {
        DWORD pkt = ++g_fakePacket[user & 3];
        ZeroMemory(st, sizeof(*st));
        st->dwPacketNumber = pkt;
    }
    return r;
}
// One hook function per DLL (each needs its own trampoline).
// re-entry guard: a foreign hook chaining back into ours is a loop - refuse the call instead of overflowing the stack
static thread_local int t_vkDepth = 0;
struct VkDepth { VkDepth() { ++t_vkDepth; } ~VkDepth() { --t_vkDepth; } };
static void VkLoop() { static volatile long n = 0; if (_InterlockedIncrement(&n) <= 3) LOGE("vrkit input: RECURSION in XInputGetState hook (thread %lu) - call refused", GetCurrentThreadId()); }
template <int N> DWORD WINAPI HkGet(DWORD u, XINPUT_STATE* s) { if (t_vkDepth > 3) { VkLoop(); return ERROR_DEVICE_NOT_CONNECTED; } VkDepth d; return Filter(g_mods[N].orig(u, s), u, s); }
template <int N> DWORD WINAPI HkGetEx(DWORD u, XINPUT_STATE* s) { if (t_vkDepth > 3) { VkLoop(); return ERROR_DEVICE_NOT_CONNECTED; } VkDepth d; return Filter(g_mods[N].origEx(u, s), u, s); }
void* const kHk[3] = { (void*)HkGet<0>, (void*)HkGet<1>, (void*)HkGet<2> };
void* const kHkEx[3] = { (void*)HkGetEx<0>, (void*)HkGetEx<1>, (void*)HkGetEx<2> };

// polling state
WORD g_prevButtons = 0; bool g_prevKeys[256] = {};
DWORD g_chordStart = 0; bool g_chordFired = false;
DWORD g_tapSeen[16] = {}, g_tapQuietUntil = 0;   // tapped chord (see Poll)
DWORD g_repeatAt[4] = {}; uint32_t g_repeatDir = 0;
} // namespace

void Configure(const Settings& s) { g_set = s; }

bool InstallHooks() {
    bool any = false;
    for (int k = 0; k < 3; ++k) {
        Hooked& h = g_mods[k];
        HMODULE m = GetModuleHandleW(h.dll);
        if (!m) continue;
        if (!h.target) {
            void* p = (void*)GetProcAddress(m, "XInputGetState");
            if (p && MH_CreateHook(p, kHk[k], (void**)&h.orig) == MH_OK && MH_EnableHook(p) == MH_OK) {
                h.target = p; Log("vrkit: hooked XInputGetState in %ls", h.dll);
                // Our own reader must not go through our hook: the hook hides L3+R3 (and everything while the menu is
                // open), so reading the export we loaded ourselves would never see the chord (GreedFall on the user's PC).
                if (g_read == (GetStateFn)p) { g_read = h.orig; Log("vrkit: pad reader moved to the unhooked original"); }
            }
            void* px = (void*)GetProcAddress(m, (LPCSTR)100);   // XInputGetStateEx (ordinal 100, includes the guide button)
            if (px && px != p && MH_CreateHook(px, kHkEx[k], (void**)&h.origEx) == MH_OK && MH_EnableHook(px) == MH_OK) h.targetEx = px;
        }
        if (h.orig && !g_read) g_read = h.orig;
        any = any || h.target;
    }
    if (!g_read) {   // the game hasn't loaded XInput (yet): read the pad ourselves
        HMODULE m = LoadLibraryW(L"xinput1_4.dll");
        if (!m) m = LoadLibraryW(L"xinput1_3.dll");
        if (m) g_read = (GetStateFn)GetProcAddress(m, "XInputGetState");
    }
    return any;
}

void SetBlocking(bool b) { if (g_block && !b) g_untilRelease = true; g_block = b; }
volatile bool g_chordDown = false;   // all chord buttons down right now (before the hold time)
bool GameInputBlocked() { return g_block || g_chordHeld || g_untilRelease || g_chordDown; }
volatile LONG g_injected = 0;
void Inject(uint32_t b) { InterlockedOr(&g_injected, (LONG)b); }
volatile int g_padUser = -1;
int PadUser() { return g_padUser; }

uint32_t Poll() {
    uint32_t out = (uint32_t)InterlockedExchange(&g_injected, 0);
    // --- gamepad (first connected)
    XINPUT_STATE st = {}; bool pad = false;
    int padUser = -1;
    // all 4 XInput slots: the first connected one gives sticks / triggers, buttons are combined (Steam Input and
    // controller tools may put the real pad and a virtual one on different slots)
    if (g_read) for (DWORD u = 0; u < 4; ++u) {
        XINPUT_STATE s2 = {};
        if (g_read(u, &s2) != ERROR_SUCCESS) continue;
        if (!pad) { st = s2; pad = true; padUser = (int)u; }
        else {
            st.Gamepad.wButtons |= s2.Gamepad.wButtons;
            st.Gamepad.bLeftTrigger = st.Gamepad.bLeftTrigger > s2.Gamepad.bLeftTrigger ? st.Gamepad.bLeftTrigger : s2.Gamepad.bLeftTrigger;
            st.Gamepad.bRightTrigger = st.Gamepad.bRightTrigger > s2.Gamepad.bRightTrigger ? st.Gamepad.bRightTrigger : s2.Gamepad.bRightTrigger;
        }
    }
    g_padUser = padUser;
    {   // diagnostics: what the menu sees from the pad (first changes only)
        static WORD lastB = 0xFFFF; static int lastU = -2, n = 0;
        WORD rb = pad ? st.Gamepad.wButtons : 0;
        if ((rb != lastB || padUser != lastU) && n < 80) {
            n++;
            Log("vrkit: pad user %d buttons %04X triggers %u/%u (reader %p)", padUser, rb, pad ? st.Gamepad.bLeftTrigger : 0,
                pad ? st.Gamepad.bRightTrigger : 0, (void*)g_read);
        }
        lastB = rb; lastU = padUser;
    }
    WORD b = pad ? st.Gamepad.wButtons : 0;
    float lt = pad ? st.Gamepad.bLeftTrigger / 255.f : 0.f, rt = pad ? st.Gamepad.bRightTrigger / 255.f : 0.f;
    // stick as d-pad
    if (pad) {
        const SHORT dz = 16000;
        if (st.Gamepad.sThumbLY > dz) b |= XINPUT_GAMEPAD_DPAD_UP;
        if (st.Gamepad.sThumbLY < -dz) b |= XINPUT_GAMEPAD_DPAD_DOWN;
        if (st.Gamepad.sThumbLX > dz) b |= XINPUT_GAMEPAD_DPAD_RIGHT;
        if (st.Gamepad.sThumbLX < -dz) b |= XINPUT_GAMEPAD_DPAD_LEFT;
    }
    // after closing, blocking ends once the pad is released (also for games that never call XInput)
    if (g_untilRelease && !g_block && pad && !st.Gamepad.wButtons && st.Gamepad.bLeftTrigger < 30 && st.Gamepad.bRightTrigger < 30)
        g_untilRelease = false;
    if (g_untilRelease && !pad) g_untilRelease = false;
    WORD pressed = b & ~g_prevButtons;
    g_prevButtons = b;
    // --- open / close chord (default L3 + R3)
    DWORD now = GetTickCount();
    bool chord = pad && (g_set.chordButtons || g_set.chordTriggers);
    if (g_set.chordButtons) chord = chord && (st.Gamepad.wButtons & g_set.chordButtons) == g_set.chordButtons;
    if (g_set.chordTriggers) chord = chord && lt > g_set.triggerThreshold && rt > g_set.triggerThreshold;
    if (pad && g_set.chordButtonsAlt && g_set.chordButtonsAlt != g_set.chordButtons &&
        (st.Gamepad.wButtons & g_set.chordButtonsAlt) == g_set.chordButtonsAlt)
        chord = true;   // the alternative chord (held)
    g_chordDown = chord;
    if (chord) {
        if (!g_chordStart) g_chordStart = now;
        if (!g_chordFired && now - g_chordStart >= (DWORD)g_set.chordHoldMs) {
            g_chordFired = true; out |= Toggle;
            for (auto& t : g_tapSeen) t = 0;   // the release must not count as a tap
            g_tapQuietUntil = now + 700;
        }
        g_chordHeld = g_chordFired;   // after it fired, keep the game from seeing the triggers until released
    } else { g_chordStart = 0; g_chordFired = false; g_chordHeld = false; }
    // Tapped chord: some pads / Steam Input layouts turn stick clicks into ~10 ms pulses, so the buttons are never
    // down together for the hold time (seen: Fable Anniversary, L3 and R3 each 10 ms, never at once). Every chord
    // button seen down within 300 ms of each other also toggles (then 700 ms quiet so one tap pair = one toggle).
    if (pad && g_set.chordButtons && !g_chordFired) {
        WORD down = st.Gamepad.wButtons & g_set.chordButtons;
        bool all = true;
        for (int i = 0; i < 16; ++i) {
            WORD bit = (WORD)(1u << i);
            if (!(g_set.chordButtons & bit)) continue;
            if (down & bit) g_tapSeen[i] = now;
            if (!g_tapSeen[i] || now - g_tapSeen[i] > 300) all = false;
        }
        if (all && !chord && (int)(now - g_tapQuietUntil) >= 0) {   // (held together: the hold rule above handles it)
            out |= Toggle;
            g_tapQuietUntil = now + 700;
            for (auto& t : g_tapSeen) t = 0;
            static int nlog = 0;
            if (nlog++ < 5) Log("vrkit: menu chord tapped (buttons seen within 300 ms)");
        }
    }
    // --- keyboard (every key is queried each frame so its edge state stays right). Esc is not used: the
    // game would see it too (keyboard input is not blocked) and most games open their own menu on it.
    auto key = [&](int vk) { bool d = (GetAsyncKeyState(vk) & 0x8000) != 0; bool p = d && !g_prevKeys[vk]; g_prevKeys[vk] = d; return p; };
    auto held = [&](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    bool kTog = g_set.keyboardToggleVk ? key(g_set.keyboardToggleVk) : false;
    bool kRet = key(VK_RETURN), kBack = key(VK_BACK), kPgUp = key(VK_PRIOR), kPgDn = key(VK_NEXT), kDel = key(VK_DELETE);
    if (kTog) out |= Toggle;
    if ((pressed & XINPUT_GAMEPAD_A) || kRet) out |= Accept;
    if ((pressed & XINPUT_GAMEPAD_B) || kBack) out |= Back;
    if ((pressed & XINPUT_GAMEPAD_LEFT_SHOULDER) || kPgUp) out |= PagePrev;
    if ((pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER) || kPgDn) out |= PageNext;
    if ((pressed & XINPUT_GAMEPAD_Y) || kDel) out |= Reset;
    // directions with auto-repeat (first repeat after 400 ms, then every 90 ms)
    uint32_t dirs = 0;
    if (b & XINPUT_GAMEPAD_DPAD_UP || held(VK_UP)) dirs |= Up;
    if (b & XINPUT_GAMEPAD_DPAD_DOWN || held(VK_DOWN)) dirs |= Down;
    if (b & XINPUT_GAMEPAD_DPAD_LEFT || held(VK_LEFT)) dirs |= Left;
    if (b & XINPUT_GAMEPAD_DPAD_RIGHT || held(VK_RIGHT)) dirs |= Right;
    const uint32_t bits[4] = { Up, Down, Left, Right };
    for (int k = 0; k < 4; ++k) {
        if (!(dirs & bits[k])) { g_repeatAt[k] = 0; continue; }
        if (!g_repeatAt[k]) { out |= bits[k]; g_repeatAt[k] = now + 400; }
        else if ((int)(now - g_repeatAt[k]) >= 0) { out |= bits[k]; g_repeatAt[k] = now + 90; }
    }
    return out;
}

} } // namespace vrkit::input
