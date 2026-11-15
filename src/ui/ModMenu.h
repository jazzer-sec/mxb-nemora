#pragma once

namespace mxb {
namespace ui {

// Draws the overlay. Call once per frame, inside an ImGui frame, from the
// game's main thread.
void Render();

bool& IsOpen();

// Tab order matches the sidebar.
enum Tab {
    kTabTyres = 0,
    kTabSuspension,
    kTabEngine,
    kTabTransmission,
    kTabCrash,
    kTabVisuals,
    kTabWorld,
    kTabCar,
    kTabChams,
    kTabStatus,
    kTabCount,
};

}  // namespace ui
}  // namespace mxb
