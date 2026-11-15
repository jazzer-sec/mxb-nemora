#include "Visuals.h"

#include <windows.h>

#include <chrono>
#include <unordered_map>

#include "BikeData.h"
#include "Offsets.h"
#include "Process.h"
#include "host/Log.h"

namespace mxb {
namespace {

VisualSettings g_settings;
std::chrono::steady_clock::time_point g_lastGesture{};

// Gesture keys, matching the game's own control defaults and the order of
// kGestureNames.
constexpr WORD kGestureKeys[kGestureCount] = {
    'M',  // raise arm
    'N',  // complaint
    'H',  // angry
    'T',  // chat
    'G',  // trick
};

void SendGesture(WORD vk) {
    // The game reads the keyboard through DirectInput as well as window
    // messages, and SendInput injects below both, so this reaches either path.
    INPUT in[2] = {};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = vk;
    in[0].ki.wScan = static_cast<WORD>(MapVirtualKeyA(vk, MAPVK_VK_TO_VSC));
    in[1] = in[0];
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

}  // namespace

VisualSettings& Visuals() { return g_settings; }

void VisualsRestore() {
    // Nothing persistent to undo: BeginViewFrustum writes the camera FOV and
    // EndViewFrustum puts it back on the same call, so no override ever outlives
    // the frustum build that used it.
}

void VisualsTick() {
    if (!g_settings.autoGesture) return;

    // Trigger 1 ("on overtake") needs the race position field, which is not
    // mapped yet, so only the crash trigger does anything.
    if (g_settings.gestureTrigger != 0) return;

    int idx = g_settings.gesture;
    if (idx < 0 || idx >= kGestureCount) idx = 0;

    static std::unordered_map<void*, int32_t> lastFlag;
    const auto now = std::chrono::steady_clock::now();
    const auto cooldown = std::chrono::duration<float>(
        g_settings.gestureCooldown < 0.1f ? 0.1f : g_settings.gestureCooldown);

    for (auto& bike : LoadedBikes()) {
        const int32_t flag = bike.crashedFlag();
        int32_t& prev = lastFlag[bike.raw()];
        const bool justCrashed = (flag != 0 && prev == 0);
        prev = flag;

        if (!justCrashed) continue;
        if (now - g_lastGesture < cooldown) continue;

        g_lastGesture = now;
        SendGesture(kGestureKeys[idx]);
        host::Logf("auto gesture '%s' fired for bike %p", kGestureNames[idx], bike.raw());
    }
}

}  // namespace mxb
