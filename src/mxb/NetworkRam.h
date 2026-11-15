#pragma once
#include <cstdint>

namespace mxb::networkram {
struct TargetScan {
    int inUse = 0, remote = 0, receiving = 0, snapshots = 0, invalidPose = 0;
};
struct Status {
    bool ready, enabled;
    const char* note;
    uint64_t changed, skipped;
    int targetSlot, targetId, targets;
    TargetScan scan{};
    float leadMilliseconds = 220.0f;
    bool bikeOnly = false;
    float outgoingYaw = 0;
    float outgoingYawRate = 0;
};
Status GetStatus();
void SetHookReady(bool ready, const char* note);
void SetEnabled(bool enabled);
void SetLeadMilliseconds(float milliseconds);
void SetBikeOnly(bool bikeOnly);
// Called only at the verified local gameplay-client send site. Writes solely
// to its serialized state; the game-owned source snapshot is read-only.
void OnSerialized(void* outgoing, const void* snapshot);
}
