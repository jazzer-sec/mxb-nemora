#pragma once

namespace mxb {

// Chams: recolour the rider and the bike.
//
// Mat_SetupMaterial (0x14024B6E0) is not a pre-draw state setter, it IS the
// draw call: it carries every glMaterialfv in the renderer and it increments
// per-draw statistics as it goes, including a texture-switch counter and a
// realloc-grown table of unique texture ids. The first version set the material
// AFTER calling the original and nothing happened, because by then the mesh was
// already on screen.
//
// So instead of overriding afterwards, the game's own material calls are
// rewritten as it makes them: ChamsBegin arms a substitution, the original runs
// and every glMaterialfv/glMaterialf/glBindTexture it issues passes through the
// GL hooks, and ChamsEnd disarms. Nothing is written behind the renderer's
// back and the timing cannot be wrong.
//
// IMPORTANT for anything added here: all of this runs once per MESH, hundreds
// of times a frame. An earlier version called IsReadable (a VirtualQuery) on
// this path and cost most of the frame rate. No syscalls, no allocation, no
// locks. Pointers need no validating either -- the renderer has already
// dereferenced them.

enum ChamStyle {
    kChamFlat = 0,  // unlit solid colour
    kChamGlow,      // solid, brightness breathing
    kChamRainbow,   // hue cycling
    kChamMetallic,  // dark base, hard specular highlight
    kChamPearl,     // lit base with a soft coloured sheen
    kChamStyleCount,
};

inline const char* const kChamStyles[] = {"Flat", "Glow", "Rainbow", "Metallic", "Pearl"};

struct ChamsPart {
    bool  enabled  = false;
    int   style    = kChamFlat;
    float color[4] = {0.35f, 0.75f, 1.00f, 1.0f};
};

struct ChamsSettings {
    ChamsPart rider;
    ChamsPart bike;

    // Also unbind the texture for a chammed mesh. Material alone can still lose
    // to a shader that samples the skin and multiplies, so this is what makes a
    // cham properly flat rather than a faint tint.
    bool forceFlat = true;

    // The +6392 object is almost certainly the rider: present only on vehicles,
    // and given its own renderer, which is what an animated character needs and
    // an ordinary model does not. Swaps the two if that inference is backwards.
    bool swapParts = false;
};

ChamsSettings& Chams();

// The entity type test that used to gate this coloured the track instead of the
// bike: the pointer Mat_SetupMaterial passes is not the scene entity that field
// belongs to, or type 1 does not mean there what it means in the render loop.
// The vehicle list is rebuilt each frame from the collection the render loops
// themselves walk, so membership cannot make that mistake. The rider needs no
// entity check at all -- Render_RiderObject only ever draws a vehicle's +6392
// object, so its return address is identification enough.

// --- called from the Mat_SetupMaterial hook, around the original ----------
// Returns true when this mesh is being chammed, in which case the GL hooks
// below will substitute until ChamsEnd.
bool ChamsBegin(const void* entity, bool fromRiderRenderer);
void ChamsEnd();

// --- called from the GL hooks --------------------------------------------
// Fills `out` and returns true when this material property should be replaced.
bool ChamsMaterialfv(unsigned pname, float out[4]);
// Returns true and fills `out` when glMaterialf's value should be replaced.
bool ChamsMaterialf(unsigned pname, float* out);
// True while a chammed mesh should have its texture unbound.
bool ChamsSuppressTexture();

// Per-frame, from the overlay: advances the animated styles and publishes the
// counters.
void ChamsTick();

struct ChamsStatus {
    int  riderDraws = 0;       // meshes chammed last frame
    int  bikeDraws = 0;
    int  riderMeshes = 0;      // meshes from Render_RiderObject
    int  vehicleMeshes = 0;    // meshes whose owner was a known vehicle
    int  totalMeshes = 0;      // every mesh the hook saw
    int  materialCalls = 0;    // material properties actually substituted
    int  sceneEntities = 0;    // everything in the scene collection
    int  typeOneEntities = 0;  // how many are type 1 -- NOT the same as vehicles
    int  vehicleEntities = 0;  // and how many pass the game's full predicate
    bool hooked = false;       // Mat_SetupMaterial hook installed
    bool glHooked = false;     // glMaterialfv hook installed
};
ChamsStatus ChamsState();
void ChamsSetHooked(bool matSetup, bool glMaterial);

}  // namespace mxb
