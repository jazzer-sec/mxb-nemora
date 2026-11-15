#pragma once

namespace mxb {

// "Car mode": steering rotates the bike directly instead of leaning it.
//
// The steer input drives the chassis body's YAW RATE, the horizontal velocity is
// steered onto the bike's own forward axis so it goes where it points, and roll
// and pitch rates are bled off so it stays upright.
//
// This writes the rigid body's angular and linear velocity. The physics
// library's own setters (Body_SetAngVel / Body_SetLinVel) do exactly that as
// plain float writes with no side effects, so it is a supported operation done
// with memory writes rather than by calling game code. Working at the velocity
// level also means the solver integrates the result instead of being
// contradicted by it -- writing the transform directly is the shape of mistake
// that made never-crash fling the bike across the map.
//
// Two earlier versions were wrong, both recorded in the README:
//   * Built from the game's own rider aids. A bike turns BY rolling, so a
//     self-balancing assist removes the turning mechanism instead of replacing
//     it, and the bike got HARDER to turn.
//   * Offered an "invert steering" toggle. Yawing against the steering geometry
//     holds the tyres at roughly twice the steer angle of slip forever, which
//     diverges the solve and kills the game. There is no such option now: the
//     sign is fixed and tested.
struct CarModeSettings {
    bool enabled = false;

    // Yaw rate at full lock, degrees per second.
    float yawRate = 110.0f;

    // Speed at which full yaw rate becomes available, m/s. Below it the rate
    // ramps in, so the bike steers like a car -- a turn radius -- rather than
    // spinning on the spot while stationary.
    float speedRef = 6.0f;

    // How hard velocity is pulled onto the bike's forward axis, per frame.
    // 1.0 is on rails, 0 leaves the stock sliding behaviour untouched.
    float grip = 0.90f;

    // How hard roll and pitch rates are bled off each frame, 0..1.
    float upright = 0.80f;

    // Apply to AI bikes as well as the player's.
    bool allBikes = true;
};

CarModeSettings& CarMode();

// Per-frame, from the overlay. Deliberately runs on the render frame rather than
// from a Bike_Update hook: it only reads and writes memory, so it needs no new
// hook and no guess at that function's calling convention. An angular velocity
// persists until the solver changes it, so setting it once a frame is enough.
void CarModeTick();

// What the last tick saw, for the menu.
struct CarModeStatus {
    bool        active = false;
    int         bikes = 0;
    float       steer = 0.0f;
    float       speed = 0.0f;
    float       yaw = 0.0f;
    int         fwdAxis = -1;   // which body axis turned out to be forward
    float       fwdSign = 0.0f;
    int         rejected = 0;   // writes undone by the divergence guard
    const char* note = "";
};
CarModeStatus CarModeState();

}  // namespace mxb
