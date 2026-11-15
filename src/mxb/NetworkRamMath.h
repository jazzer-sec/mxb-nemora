#pragma once
#include "SpinnyMath.h"
#include <cstdint>
#include <vector>

namespace mxb::networkram {
namespace pose = spinny::math;
constexpr uint64_t kTargetMilliseconds = 80;
constexpr double kSweepDistance = 6.0;
constexpr double kSweepSpeed = kSweepDistance * 1000.0 / kTargetMilliseconds;
constexpr double kSpinDegreesPerSecond = 2160.0; // six pose revolutions/second
constexpr double kMaxLeadSeconds = 0.5;
constexpr double kMaxLeadMetres = 12.0;
// beta21 type-9 compression clamps angular velocity to [-12,+10] rad/s.
constexpr double kWireYawRate = 10.0;
constexpr size_t kDetachedRiderOffset = 260;

struct Target {
    int slot = 0; // 1-based
    int id = 0;
    pose::Vec position{}, velocity{}, direction{};
};

// Round robin by slot, keeping a stable target for a complete sweep. Missing
// players and reused slots cause an immediate switch instead of a stale hit.
struct Cycle {
    int slot = 0, id = 0;
    uint64_t started = 0;
    void Reset() { slot = id = 0; started = 0; }
    const Target* Select(const std::vector<Target>& targets, uint64_t now) {
        if (targets.empty()) { Reset(); return nullptr; }
        if (now >= started && now - started < kTargetMilliseconds) {
            for (const auto& target : targets)
                if (target.slot == slot && target.id == id) return &target;
        }
        const Target* next = nullptr;
        for (const auto& target : targets) {
            if (target.slot > slot && (!next || target.slot < next->slot)) next = &target;
        }
        if (!next) {
            for (const auto& target : targets)
                if (!next || target.slot < next->slot) next = &target;
        }
        slot = next->slot; id = next->id; started = now;
        return next;
    }
};

inline bool Project(std::byte* state, const std::byte* snapshot, const Target& target, uint64_t elapsed,
                    double spinSeconds = 0, double leadSeconds = 0.22, bool bikeOnly = false) {
    if (!state || !snapshot || elapsed >= kTargetMilliseconds ||
        !pose::Finite(target.position) || !pose::Finite(target.velocity) ||
        !pose::Finite(target.direction) || !std::isfinite(spinSeconds) || spinSeconds < 0 ||
        !std::isfinite(leadSeconds) || leadSeconds < 0 || leadSeconds > kMaxLeadSeconds) return false;
    const double horizontal = std::hypot(target.direction[0], target.direction[2]);
    if (horizontal < 0.01 || std::abs(target.direction[1]) > 0.01) return false;
    const auto oldBike = pose::Rotation(snapshot,4);
    const auto oldRider = pose::Rotation(snapshot,0x438);
    if (!pose::ValidRotation(oldBike) || !pose::ValidRotation(oldRider)) return false;
    // Start broadside and spin around world +Y. Time is shared across targets,
    // so switching riders does not restart the spin phase.
    const double phase = std::fmod(spinSeconds,360.0/kSpinDegreesPerSecond)*kSpinDegreesPerSecond;
    const double yaw = std::remainder(std::atan2(target.direction[0],target.direction[2])/pose::kRadians + 90.0 + phase,360.0);
    const pose::Vec original{pose::Read(state,0),pose::Read(state,4),pose::Read(state,8)};
    if (!pose::Finite(original)) return false;
    for (size_t offset = 264; offset < pose::kStateBytes; offset += 4)
        if (!std::isfinite(pose::Read(state,offset))) return false;
    pose::Vec lead{};
    // Predict horizontal travel only. Each outgoing update uses the target's
    // current snapshot Y exactly, including during jumps and falls.
    for (int axis : {0,2}) lead[axis] = target.velocity[axis]*leadSeconds;
    const double leadLength = std::hypot(lead[0],lead[1],lead[2]);
    if (leadLength > kMaxLeadMetres)
        for (double& value : lead) value *= kMaxLeadMetres/leadLength;
    std::array<std::byte,pose::kStateBytes> copy{};
    std::memcpy(copy.data(),state,copy.size());
    const double offset = kSweepDistance/2 - kSweepSpeed * elapsed/1000.0;
    for (int axis = 0; axis < 3; ++axis) {
        const double direction = target.direction[axis]/horizontal;
        const double position = target.position[axis] + lead[axis] + direction*offset;
        const double velocity = target.velocity[axis] - direction*kSweepSpeed;
        if (std::abs(position) > 1000000 || std::abs(velocity) > 200) return false;
        pose::Write(copy.data(),axis*4,position);
        pose::Write(copy.data(),12 + axis*4,velocity);
        pose::Write(copy.data(),36 + axis*4,axis == 1 ? kWireYawRate : 0);
    }
    pose::Write(copy.data(),24,0);
    pose::Write(copy.data(),28,yaw);
    pose::Write(copy.data(),32,0);
    // A detached rider uses the original absolute rider pose, but proxy update
    // 0x168040 also zeros its bike's angular velocity. Default to preserving
    // the source flag so attached riders retain receiver-side spin. Bike-only
    // mode remains available with that known limitation.
    if (bikeOnly) {
        const int32_t detached = 1;
        std::memcpy(copy.data()+kDetachedRiderOffset,&detached,sizeof(detached));
    }
    std::memcpy(state,copy.data(),copy.size());
    return true;
}
}  // namespace mxb::networkram
