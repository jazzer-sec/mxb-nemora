#pragma once

namespace mxb {
namespace ui {

// China hat: a cone floating over each rider. Drawn as real 3D geometry --
// a world-space ring plus an apex, each point pushed through the engine's own
// screen matrix -- so it keeps correct perspective and leans as the camera
// moves, rather than being a flat screen-space triangle that happens to track
// the rider. It goes in the foreground draw list with no depth testing, so it
// shows through terrain, which is the whole point of the thing.
struct ChinaHatSettings {
    bool  enabled    = false;
    float height     = 1.55f;  // metres above the bike origin, i.e. roughly head height
    float radius     = 0.34f;
    float coneHeight = 0.42f;
    int   style      = 0;      // index into kHatStyles
    float color[4]   = {0.56f, 0.65f, 0.90f, 0.85f};
    bool  outline    = true;
};

// Solid is the picked colour. The other three animate: Rainbow cycles the whole
// hat, Pulse breathes its brightness, and Hue sweep runs the hue around the
// cone so it reads as spinning.
inline const char* const kHatStyles[] = {"Solid", "Rainbow", "Pulse", "Hue sweep"};
inline constexpr int kHatStyleCount = 4;

ChinaHatSettings& ChinaHat();

// Per-frame, from the overlay, before the menu is drawn.
void DrawWorldOverlays();

}  // namespace ui
}  // namespace mxb
