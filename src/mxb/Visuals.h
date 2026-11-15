#pragma once

namespace mxb {

struct VisualSettings {
    // Camera FOV, in the game's own units: degrees, referenced to 4:3, the same
    // number a cameras.cfg entry carries. View_BuildFrustum widens it for wider
    // displays by aspect/1.3333, so 75 here is not 75 degrees on a 16:9 screen.
    //
    // Applied by overriding camera+4 around View_BuildFrustum. Two earlier
    // approaches were wrong: writing camera%d/fov in the camera table is never
    // read at render time, and rescaling the planes in a glFrustum hook widened
    // the projection but left the culling planes, far extents and two of the
    // three main-view passes at the stock FOV -- which is what broke the
    // periphery. NOTES.md has the whole chain.
    bool  fovOverride = false;
    float fov         = 75.0f;

    // Fires one of the bike's gesture bindings automatically. Keys are the
    // game's own control defaults.
    bool  autoGesture     = false;
    int   gesture         = 0;  // index into kGestureNames
    int   gestureTrigger  = 0;  // index into kGestureTriggers
    float gestureCooldown = 2.0f;
};

// Gesture bindings, in the order the game's controls screen lists them.
// Win gesture is on XInput D-pad right by default and has no keyboard key, so
// it is not offered here.
inline const char* const kGestureNames[] = {"Raise arm", "Complaint", "Angry", "Chat", "Trick"};
inline constexpr int kGestureCount = 5;

inline const char* const kGestureTriggers[] = {"On crash", "On overtake (not wired)"};
inline constexpr int kGestureTriggerCount = 2;

VisualSettings& Visuals();

// Per-frame, from the overlay. Applies the FOV override and services auto
// gesture. Cheap when nothing is enabled.
void VisualsTick();

// Nothing persistent to undo -- the FOV override is restored inside the
// View_BuildFrustum hook on the same call that applies it, so dropping the hook
// is already enough. Kept so the unload path has one place to call.
void VisualsRestore();

}  // namespace mxb
