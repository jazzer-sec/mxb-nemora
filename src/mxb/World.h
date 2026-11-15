#pragma once

namespace mxb {

// ---------------------------------------------------------------------------
// World look
// ---------------------------------------------------------------------------
// Every value here is applied by intercepting the engine's own setter at the
// moment it hands the value to GL, rather than by writing the global behind it.
// Those globals are recomputed per frame from the track's weather and time of
// day, so a write would be overwritten; an interception cannot be, and it needs
// nothing saved or restored because the stock value is still what the engine
// holds.
struct WorldSettings {
    bool enabled = false;

    // Blends the light, sky and fog toward night. Deliberately a blend on top of
    // the manual knobs below rather than a preset that overwrites them, so
    // switching it off puts everything back without clobbering anything set by
    // hand.
    bool  night       = false;
    float nightAmount = 0.85f;

    // Scales GL_LIGHT0's AMBIENT term, which is where this engine keeps the
    // world's light level: GL_LIGHT_MODEL_AMBIENT is hard-coded to zero, so
    // LIGHT0's ambient is what lights everything. Filtering a glLightfv hook on
    // GL_DIFFUSE instead never fires -- the pname the game passes is 0x1200.
    float lightLevel = 1.0f;

    // glClearColor: the colour the framebuffer is cleared to, which is BEHIND
    // the track's sky. Most tracks draw a sky dome over the whole view, so this
    // is usually invisible -- it is not a skybox tint. Tinting the actual sky
    // needs the sky draw path, which is the same unfinished work as chams.
    bool  overrideClear = false;
    float clearColor[3] = {0.04f, 0.06f, 0.12f};

    int   fogMode          = 0;     // index into kFogModes
    float fogDensity       = 1.0f;  // multiplier on GL_FOG_DENSITY
    bool  overrideFogColor = false;
    float fogColor[3]      = {0.45f, 0.50f, 0.62f};

    // Multiplies the light, clear and fog colours. Neutral at white.
    float tint[3] = {1.0f, 1.0f, 1.0f};

    // A flat veil over the finished frame. The lighting knobs above only reach
    // what the sun lights, so this is what catches the sky texture and anything
    // unlit. It gets its own colour rather than reusing the tint: ImGui draw
    // lists blend with straight alpha, so a neutral white tint used as a veil
    // would wash the screen out instead of darkening it. Night mode drives this
    // on its own, so the slider is only needed for a deliberate colour wash.
    float filterColor[3] = {0.02f, 0.03f, 0.08f};
    float screenTint     = 0.0f;  // 0..1
};

// The veil actually drawn, after night mode has had its say: colour in rgb[3]
// and strength as the return value. Zero means draw nothing.
float ScreenFilter(float rgb[3]);

inline const char* const kFogModes[] = {"Stock", "Forced on", "Forced off"};
inline constexpr int kFogModeCount = 3;

WorldSettings& World();

// --- applied-value interceptors, called from the hooks ---------------------
// Each rewrites in place what the game is about to apply. All are no-ops while
// the world modifier is off, and all are safe to call on every frame.
void ModulateWorldLight(float rgba[4]);
void ModulateClearColor(float* r, float* g, float* b);
void ModulateFogColor(float rgb[3]);
void ModulateFogEnable(int* on);
void ModulateFogDensity(float* density);

// ---------------------------------------------------------------------------
// View / FOV
// ---------------------------------------------------------------------------
// What BeginViewFrustum did, so EndViewFrustum can undo it. The camera object
// is shared between views and frames, so an override left in it would persist.
struct ViewHookState {
    bool  main     = false;    // the player's own view, as opposed to a probe or reflection
    void* camera   = nullptr;  // non-null only if its FOV was actually overridden
    float stockFov = 0.0f;
};

// Call around the original View_BuildFrustum. Begin applies the FOV override;
// End restores it and captures the view's projection for ProjectWorldToScreen.
// Capturing in End, not Begin, is deliberate: the screen matrix does not exist
// until the original has run.
ViewHookState BeginViewFrustum(void* view);
void EndViewFrustum(void* view, const ViewHookState& state);

// True once a main view has been captured this session.
bool HaveView();

// The FOV the player's camera last loaded with, before any override. Shown in
// the menu so the slider can be read against the stock value.
float StockCameraFov();

// Projects a world point through the engine's own screen matrix. Writes pixel
// coordinates with y measured down from the top, matching ImGui, plus the
// view-space depth. False when there is no view, the point is behind the near
// plane, or the result is not finite.
bool ProjectWorldToScreen(const float world[3], float screenW, float screenH, float out[2],
                          float* depth);

}  // namespace mxb
