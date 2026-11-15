#include "NetworkRam.h"
#include <vector>
#include "NetworkRamMath.h"
#include "Process.h"
#include <chrono>
#include <mutex>

namespace mxb::networkram {
namespace {
std::mutex g_mutex;
Status g_status{false,false,"hook not installed",0,0,0,0,0};
Cycle g_cycle;
uint64_t g_spinStartedMs = 0;

template <typename T> T Read(const uint8_t* bytes, size_t offset) {
    T result{}; std::memcpy(&result,bytes+offset,sizeof(result)); return result;
}
bool ReadInt(uintptr_t address, int& value) {
    if (!address || !IsReadable(reinterpret_cast<void*>(address),sizeof(value))) return false;
    std::memcpy(&value,reinterpret_cast<void*>(address),sizeof(value)); return true;
}
std::vector<Target> Targets(int localIndex, TargetScan& scan) {
    std::vector<Target> targets;
    targets.reserve(off::kMaxPlayers-1);
    const uintptr_t slots = Rva(off::kPlayerSlots);
    for (int index = 1; slots && index <= off::kMaxPlayers; ++index) {
        if (index == localIndex) continue;
        const auto* slot = reinterpret_cast<const uint8_t*>(slots + off::kPlayerSlotStride*(index-1));
        if (!IsReadable(slot,off::kPlayerSlotSnapshot+pose::kSnapshotBytes) || !Read<int>(slot,0)) continue;
        ++scan.inUse;
        if (!Read<int>(slot,off::kPlayerSlotRemote)) continue;
        ++scan.remote;
        if (!Read<int>(slot,off::kPlayerSlotReceiving)) continue;
        ++scan.receiving;
        if (Read<int>(slot,off::kPlayerSlotSnapshot) != 1) continue;
        ++scan.snapshots;
        const auto* snapshot = reinterpret_cast<const std::byte*>(slot+off::kPlayerSlotSnapshot);
        const auto rotation = pose::Rotation(snapshot,4);
        if (!pose::ValidRotation(rotation)) { ++scan.invalidPose; continue; }
        Target target;
        target.slot = index; target.id = Read<int>(slot,off::kPlayerSlotRosterId);
        target.position = pose::Position(snapshot,4);
        for (int axis = 0; axis < 3; ++axis) target.velocity[axis] = pose::Read(snapshot,68+axis*4);
        target.direction = {rotation[2],0,rotation[8]};
        const double length = std::hypot(target.direction[0],target.direction[2]);
        if (!pose::Finite(target.position) || !pose::Finite(target.velocity) || length < 0.1) {
            ++scan.invalidPose; continue;
        }
        bool bounded = true;
        for (int axis = 0; axis < 3; ++axis)
            if (std::abs(target.position[axis]) > 999990 || std::abs(target.velocity[axis]) > 190) bounded = false;
        if (!bounded) { ++scan.invalidPose; continue; }
        for (double& value : target.direction) value /= length;
        targets.push_back(target);
    }
    return targets;
}
}
Status GetStatus() { std::lock_guard<std::mutex> lock(g_mutex); return g_status; }
void SetHookReady(bool ready, const char* note) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_status.ready = ready; g_status.note = note;
    if (!ready) {
        g_status.enabled = false; g_cycle.Reset();
        g_status.targetSlot = g_status.targetId = g_status.targets = 0; g_status.scan = {};
        g_spinStartedMs = 0;
        g_status.outgoingYaw = g_status.outgoingYawRate = 0;
    }
}
void SetEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_status.enabled = enabled && g_status.ready;
    g_cycle.Reset(); g_status.targetSlot = g_status.targetId = g_status.targets = 0;
    g_status.scan = {};
    g_spinStartedMs = 0;
    g_status.outgoingYaw = g_status.outgoingYawRate = 0;
    if (g_status.ready) g_status.note = g_status.enabled ? "waiting for players" : "ready";
}
void SetLeadMilliseconds(float milliseconds) {
    if (!std::isfinite(milliseconds)) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_status.leadMilliseconds = std::clamp(milliseconds,0.0f,static_cast<float>(kMaxLeadSeconds*1000.0));
}
void SetBikeOnly(bool bikeOnly) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_status.bikeOnly = bikeOnly;
}
void OnSerialized(void* outgoing, const void* snapshot) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_status.ready || !g_status.enabled) return;
    g_status.scan = {};
    auto skip = [&](const char* note) {
        g_status.note = note; ++g_status.skipped;
        g_status.targetSlot = g_status.targetId = g_status.targets = 0; g_cycle.Reset();
        g_status.outgoingYaw = g_status.outgoingYawRate = 0;
    };
    int client = 0, hosting = 0, local = 0;
    if (!ReadInt(Rva(off::kGameplayClientState),client) || client != 7 ||
        !ReadInt(Rva(off::kHostingSession),hosting) || hosting ||
        !ReadInt(Rva(off::kLocalPlayerIndex),local) || local < 1 || local > off::kMaxPlayers) {
        skip("waiting for gameplay client"); return;
    }
    if (!IsReadable(outgoing,pose::kStateBytes) || !IsReadable(snapshot,pose::kSnapshotBytes)) {
        skip("state unavailable"); return;
    }
    const auto targets = Targets(local,g_status.scan);
    const uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto* target = g_cycle.Select(targets,now);
    if (!target) {
        const auto& scan = g_status.scan;
        skip(!scan.inUse ? "no other occupied slots" :
             !scan.remote ? "no remote rider slots" :
             !scan.receiving ? "waiting for received players" :
             !scan.snapshots ? "waiting for player snapshots" : "remote poses rejected");
        return;
    }
    if (!g_spinStartedMs || now < g_spinStartedMs) g_spinStartedMs = now;
    if (!Project(static_cast<std::byte*>(outgoing),static_cast<const std::byte*>(snapshot),*target,now-g_cycle.started,
                 (now-g_spinStartedMs)/1000.0,g_status.leadMilliseconds/1000.0,g_status.bikeOnly)) {
        skip("invalid outgoing pose"); return;
    }
    ++g_status.changed; g_status.targetSlot = target->slot; g_status.targetId = target->id;
    g_status.outgoingYaw = pose::Read(static_cast<const std::byte*>(outgoing),28);
    g_status.outgoingYawRate = pose::Read(static_cast<const std::byte*>(outgoing),40);
    g_status.targets = static_cast<int>(targets.size());
    g_status.note = Read<int>(static_cast<const uint8_t*>(outgoing),kDetachedRiderOffset) ?
        "detached rider limits spin" : "cycling players";
}
}
