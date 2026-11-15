#pragma once

namespace mxb {
namespace crash {

// Rider_Update (0x1401A8480) detaches the rider when any of four force thresholds
// is exceeded. Two of those constants are referenced only by this check, so we
// overwrite them in place. The other two are shared with unrelated code, so we
// leave the constants alone and repoint the instruction's RIP-relative
// displacement at our own copies instead.
//
// tolerance > 1.0 means the rider holds on through harder hits.
// Returns false if any site did not match the expected bytes (wrong build).
bool Apply(float tolerance);

// Undo everything Apply touched. Safe to call when nothing is patched.
void Restore();

bool IsApplied();

// Tolerance that makes the detach condition unreachable, i.e. "never crash".
//
// This is deliberately the SAME mechanism as the slider, just enormous, rather
// than a patch on Rider_Detach itself. Suppressing the detach function does not
// work: the site that calls it reads
//
//     if (threshold exceeded) { Rider_Detach(a1); return 1; }
//
// so it returns out of Rider_Update before the remaining rider physics runs.
// With the function neutered the rider still stops being simulated on every
// frame the thresholds are exceeded, nothing opposes the impact, and a landing
// launches the bike into the air or NaNs the position outright.
//
// Raising the thresholds instead means the condition is simply never true and
// Rider_Update completes normally every frame. 4000 * 1e24 still fits a float
// with room to spare, so no infinity sneaks in.
inline constexpr float kNoCrashTolerance = 1e24f;

// Raising the force thresholds is necessary but not sufficient. Rider_Update has
// THREE separate exits that call Rider_Detach, and each one is a
// `Rider_Detach(a1); return 1;` that abandons the remaining rider physics for
// that frame:
//
//   1. 0x1401AD30E  the four force thresholds        -> Apply() scales them
//   2. 0x1401A85C4  rider body at or below the ground -> NOT suppressed, see below
//   3. 0x1401A8799  lean angle past 75 degrees       -> SetNoCrash() patches it
//
// (3) compares against a 75.0 that three unrelated functions also read, so the
// constant cannot be touched; its "skip the detach" conditional jump is turned
// into an unconditional one instead -- one opcode byte, verified before writing
// and restored on undo.
//
// (2) is left alone on purpose. It is not a crash rule: it means the rider is
// inside the terrain, and detaching is the only response the solver models for
// that. Suppressing it leaves the rider embedded and something downstream
// ejects it, which is what put the bike in the air with the camera under the
// ground after a gentle landing. Body-to-dirt contact therefore still ends the
// run even with Never crash on.
bool SetNoCrash(bool enabled);
bool NoCrashActive();

// False until Apply has run once; true if the byte pattern at every site matched.
bool SitesVerified();

}  // namespace crash
}  // namespace mxb
