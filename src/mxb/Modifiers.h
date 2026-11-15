#pragma once
#include <cstdint>

// TESTING BUILD SWITCH -- set back to 0 before this is used for real.
//
// 1 disables the offline guard, so the modifiers write whether or not the
// session is singleplayer, and turns every section on by default so nothing has
// to be enabled by hand first. Both exist to get the physics path exercised; the
// guard is the thing that keeps this module singleplayer-only, so this must not
// ship enabled.
#ifndef MXB_TESTING_NO_GUARDS
#define MXB_TESTING_NO_GUARDS 1
#endif

namespace mxb {

// Every knob is a multiplier on the bike's own loaded value, so one setting means
// the same thing on a 125 two-stroke and a 450 four-stroke. 1.0 is stock.
struct GripSettings {
    bool  enabled      = MXB_TESTING_NO_GUARDS ? true : false;
    float peakLong     = 1.0f;  // tyre p_Dx1
    float peakLat      = 1.0f;  // tyre p_Dy1
    float surfaceDry   = 1.0f;  // per-surface dry_grip
    float surfaceWet   = 1.0f;  // per-surface wet_grip
    float carcassSpring= 1.0f;  // tyre SpringBase
    float carcassDamper= 1.0f;  // tyre Damper
};

struct SuspensionSettings {
    bool  enabled       = MXB_TESTING_NO_GUARDS ? true : false;
    float frontSpring   = 1.0f;  // Front_Suspension/Spring base + step
    float rearSpring    = 1.0f;  // Rear_Suspension/Spring  base + step
    float frontDamping  = 1.0f;  // front bump + rebound thresholds
    float rearDamping   = 1.0f;  // rear  bump + rebound thresholds
    float bumpStop      = 1.0f;  // rear BumpStop spring + damping
    float topOut        = 1.0f;  // front/rear TopOutSpring/K
};

struct EngineSettings {
    bool  enabled       = MXB_TESTING_NO_GUARDS ? true : false;
    float torque        = 1.0f;  // whole torque curve, every mapping
    float engineBraking = 1.0f;  // brake curve, every mapping
    float clutchTorque  = 1.0f;  // engine/Clutch/MaxTorque

    // Horsepower, as distinct from torque. Power is torque times angular
    // velocity, so letting the engine rev further raises peak power without
    // changing the torque it makes at any given rpm. Scales engine/MaxRPM,
    // engine/ShiftRPM and every engine/Limiter together, because raising the
    // ceiling and leaving the limiter alone just means hitting the limiter.
    float revRange = 1.0f;

    // Top speed, through aerodynamic drag. Applied to CdA as stock/(mult*mult),
    // since drag goes as v^2 and terminal velocity as 1/sqrt(CdA) -- so the
    // number on the slider is the speed multiple it is actually worth.
    //
    // Only bites while the bike is drag-limited. One that revs out in top gear
    // is gear-limited instead, and then this does nothing: raise the rev range
    // or the gearing.
    float topSpeed = 1.0f;
};

struct TransmissionSettings {
    bool  enabled        = MXB_TESTING_NO_GUARDS ? true : false;
    float gearRatio      = 1.0f;  // every gearbox ratio (higher = taller)
    float finalDrive     = 1.0f;  // rear sprocket teeth; quantised to whole teeth
    float shiftSpeed     = 1.0f;  // >1 = faster: divides up/down shift time
    float clutchSpeed    = 1.0f;  // >1 = faster: divides engage/disengage time
    float drivelineEff   = 1.0f;  // Driveline/Efficiency, clamped to <= 1.0
    float drivelineInertia = 1.0f;// first/second axis inertia
};

struct CrashSettings {
    bool  enabled   = MXB_TESTING_NO_GUARDS ? true : false;
    float tolerance = 1.0f;   // scales all four detach thresholds
    bool  noCrash   = false;  // raise the thresholds out of reach
    // Puts the rider back on after a crash instead of preventing it. This is the
    // only thing that covers body-to-dirt contact, which cannot be suppressed:
    // see CrashPatch.h.
    bool  autoRecover  = false;
    float recoverDelay = 0.5f;  // seconds on the floor before being put back
};

struct Settings {
    // No longer a user-facing switch; each section has its own toggle. Kept so
    // everything can still be killed from one place in code.
    bool masterEnabled = true;
    GripSettings         grip;
    SuspensionSettings   suspension;
    EngineSettings       engine;
    TransmissionSettings transmission;
    CrashSettings        crash;
};

// Live settings the menu edits.
Settings& Get();

// Call once per frame from the game's main thread (the overlay hook).
//
// Snapshots each bike's stock values the first time it is seen, then writes
// stock * multiplier. Writing from the snapshot rather than the current value
// keeps it idempotent, so running every frame never compounds.
//
// Does nothing while the session guard says writes are blocked, and restores
// every bike it has touched when the master toggle or a section is turned off.
void Tick();

// Puts every bike back to its loaded values and drops the snapshots.
void RestoreAll();

// For the menu's status line.
struct Status {
    int         recoveries    = 0;  // riders put back since injection
    bool        moduleFound   = false;
    bool        writesBlocked = true;
    int         bikesSeen     = 0;
    int         bikesPatched  = 0;
    const char* note          = "";
};
Status GetStatus();

}  // namespace mxb
