#include "CarMode.h"

#include <chrono>
#include <cmath>
#include <cstdint>

#include "BikeData.h"
#include "Offsets.h"
#include "Process.h"
#include "Session.h"
#include "host/Log.h"

namespace mxb {
namespace {

CarModeSettings g_settings;
CarModeStatus   g_status;

constexpr float kPi = 3.14159265358979323846f;

// Nothing on a motocross track legitimately exceeds these. They exist to stop a
// diverging solve from becoming a crash, not to shape the handling: 200 m/s is
// 720 km/h and 50 rad/s is eight revolutions a second.
constexpr float kMaxSpeed   = 200.0f;
constexpr float kMaxAngRate = 50.0f;

std::chrono::steady_clock::time_point g_last{};

// Which body axis points forward, resolved once by watching the bike ride
// straight. Not assumed: guessing a matrix convention is what put the china hat
// outside the map, and here the engine's own data answers the question.
int   g_fwdAxis = -1;   // 0, 1 or 2 -- a column of the orientation matrix
float g_fwdSign = 1.0f;

float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

bool Finite3(const float* v) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

// Body_AddLocalTorque computes out = (v.(f52,f53,f54), v.(f56,f57,f58),
// v.(f60,f61,f62)), i.e. it maps LOCAL to WORLD with those as rows. The local
// axes are therefore the matrix's COLUMNS, strided by 4 floats.
void AxisInWorld(const float* m, int axis, float out[3]) {
    out[0] = m[axis];
    out[1] = m[axis + 4];
    out[2] = m[axis + 8];
}

// Picks the axis whose horizontal projection lines up with where the bike is
// actually travelling. Only sampled while going straight and quickly, where
// "forward" and "direction of travel" cannot disagree.
void ResolveForwardAxis(const float* m, const float* lin, float speed, float steer) {
    if (g_fwdAxis >= 0) return;
    if (speed < 5.0f || std::fabs(steer) > 0.08f) return;

    const float vx = lin[0] / speed;
    const float vz = lin[2] / speed;

    int   best = -1;
    float bestSign = 1.0f;
    float bestDot = 0.0f;

    for (int axis = 0; axis < 3; ++axis) {
        float a[3];
        AxisInWorld(m, axis, a);
        const float len = std::sqrt(a[0] * a[0] + a[2] * a[2]);
        if (!(len > 1e-4f)) continue;  // a near-vertical axis has no heading
        const float dot = (a[0] / len) * vx + (a[2] / len) * vz;
        if (std::fabs(dot) > std::fabs(bestDot)) {
            bestDot = dot;
            best = axis;
            bestSign = dot < 0.0f ? -1.0f : 1.0f;
        }
    }

    // Demand near-alignment. A marginal winner means the sample was not as
    // straight as it looked, so wait for a better one.
    if (best < 0 || std::fabs(bestDot) < 0.95f) return;

    g_fwdAxis = best;
    g_fwdSign = bestSign;
    host::Logf("car mode: forward axis resolved to column %d, sign %+.0f (dot %.3f)", best,
               bestSign, bestDot);
}

}  // namespace

CarModeSettings& CarMode() { return g_settings; }
CarModeStatus CarModeState() { return g_status; }

void CarModeTick() {
    if (!g_settings.enabled) {
        g_status = CarModeStatus{};
        g_status.fwdAxis = g_fwdAxis;
        g_status.fwdSign = g_fwdSign;
        g_last = {};
        return;
    }

    if (WritesBlocked()) {
        g_status.active = false;
        g_status.note = "writes blocked (server)";
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    float dt = 0.0f;
    if (g_last.time_since_epoch().count() != 0)
        dt = std::chrono::duration<float>(now - g_last).count();
    g_last = now;

    // A long gap means paused, loading or alt-tabbed. Acting on the resume frame
    // with a stale interval is how never-crash flung the bike.
    if (!(dt > 0.0f) || dt > 0.1f) {
        g_status.note = "settling";
        return;
    }

    const float yawMax   = Clamp(g_settings.yawRate, 0.0f, 720.0f) * (kPi / 180.0f);
    const float speedRef = g_settings.speedRef < 0.5f ? 0.5f : g_settings.speedRef;
    const float grip     = Clamp(g_settings.grip, 0.0f, 1.0f);
    const float upright  = Clamp(g_settings.upright, 0.0f, 1.0f);

    int touched = 0;
    int rejected = 0;

    for (BikeData& bike : LoadedBikes()) {
        uint8_t* raw = bike.raw();

        // The ACTIVE input copy, which Bike_Update zeroes while the rider is
        // crashed or in the pit -- so a crashed rider stops steering for free.
        const double steerD = *reinterpret_cast<const double*>(raw + off::bike::kInputActiveSteer);
        if (!std::isfinite(steerD)) continue;
        const float steer = Clamp(static_cast<float>(steerD), -1.0f, 1.0f);

        void* chassis = *reinterpret_cast<void* const*>(raw + off::bike::kChassisBody);
        if (!IsReadable(chassis, off::body::kTorqueAccum + 12)) continue;

        uint8_t* b = static_cast<uint8_t*>(chassis);
        float*   lin = reinterpret_cast<float*>(b + off::body::kLinVel);
        float*   ang = reinterpret_cast<float*>(b + off::body::kAngVel);
        const float* rot = reinterpret_cast<const float*>(b + off::body::kOrientation);
        if (!Finite3(lin) || !Finite3(ang)) continue;

        // Horizontal only: +Y is up, and vertical speed has no business feeding
        // a steering ramp.
        const float speed = std::sqrt(lin[0] * lin[0] + lin[2] * lin[2]);

        ResolveForwardAxis(rot, lin, speed, steer);

        // Everything about to be written is recoverable, so a result that comes
        // back nonsense can be undone rather than left in the solver.
        const float prevLin[3] = {lin[0], lin[1], lin[2]};
        const float prevAng[3] = {ang[0], ang[1], ang[2]};

        const float ramp = Clamp(speed / speedRef, 0.0f, 1.0f);
        const float yaw  = steer * yawMax * ramp;
        if (!std::isfinite(yaw)) continue;

        // Yaw straight from the stick, and bleed off roll and pitch so the bike
        // holds itself up instead of leaning into the turn.
        ang[1] = yaw;
        ang[0] *= (1.0f - upright);
        ang[2] *= (1.0f - upright);

        // Steer the velocity onto the bike's own forward axis, at the same
        // speed. This replaces integrating a rotation from the render interval,
        // which was the source of the residual slide: the solver changes the
        // angular velocity between our writes, so the body never rotated by
        // exactly yaw * dt and the velocity drifted out of line with it. Aiming
        // at the measured heading has no dependence on the interval at all.
        if (g_fwdAxis >= 0 && speed > 0.5f && grip > 0.0f) {
            float fwd[3];
            AxisInWorld(rot, g_fwdAxis, fwd);
            fwd[0] *= g_fwdSign;
            fwd[2] *= g_fwdSign;

            const float len = std::sqrt(fwd[0] * fwd[0] + fwd[2] * fwd[2]);
            if (len > 1e-4f) {
                const float fx = fwd[0] / len;
                const float fz = fwd[2] / len;

                // Only while actually travelling forwards. Pulling a reversing
                // bike onto its forward axis would spin it round.
                if ((lin[0] * fx + lin[2] * fz) > 0.0f) {
                    lin[0] += (fx * speed - lin[0]) * grip;
                    lin[2] += (fz * speed - lin[2]) * grip;
                }
            }
        }

        // The divergence guard. The crash that the old invert option caused was
        // the solve blowing up, and a blown-up velocity is both detectable and
        // undoable right here.
        const float newSpeed = std::sqrt(lin[0] * lin[0] + lin[1] * lin[1] + lin[2] * lin[2]);
        const float newRate = std::sqrt(ang[0] * ang[0] + ang[1] * ang[1] + ang[2] * ang[2]);
        if (!Finite3(lin) || !Finite3(ang) || newSpeed > kMaxSpeed || newRate > kMaxAngRate) {
            for (int i = 0; i < 3; ++i) {
                lin[i] = prevLin[i];
                ang[i] = prevAng[i];
            }
            ++rejected;
            continue;
        }

        ++touched;
        if (touched == 1) {
            g_status.steer = steer;
            g_status.speed = speed;
            g_status.yaw   = yaw * (180.0f / kPi);
        }
        if (!g_settings.allBikes) break;
    }

    g_status.active = touched > 0;
    g_status.bikes = touched;
    g_status.rejected += rejected;
    g_status.fwdAxis = g_fwdAxis;
    g_status.fwdSign = g_fwdSign;
    g_status.note = touched ? (g_fwdAxis < 0 ? "ride straight to resolve heading" : "") : "no bike";

    if (rejected)
        host::Logf("car mode: %d write(s) undone by the divergence guard", rejected);

    static int logged = 0;
    if (touched && logged < 5) {
        ++logged;
        host::Logf("car mode: steer=%+.3f speed=%.2f yaw=%+.1f deg/s dt=%.4f bikes=%d fwd=%d",
                   g_status.steer, g_status.speed, g_status.yaw, dt, touched, g_fwdAxis);
    }
}

}  // namespace mxb
