#include "Spinny.h"
#include "SpinnyMath.h"
#include "Process.h"
#include "host/Log.h"
#include <atomic>
#include <chrono>

namespace mxb::spinny {
namespace {
std::atomic<bool> g_ready{false},g_enabled{false};
std::atomic<const char*> g_note{"hook not installed"};
std::atomic<uint64_t> g_startMs{0},g_serialized{0},g_changed{0},g_skipped{0};
std::atomic<float> g_lift{0};
uint64_t Now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}
Status GetStatus() {
    return {g_ready.load(),g_enabled.load(),g_note.load(),g_serialized.load(),g_changed.load(),g_skipped.load(),g_lift.load()};
}
void SetHookReady(bool ready,const char* note) {
    g_ready.store(ready); g_note.store(note);
    if(!ready) { g_enabled.store(false); g_lift.store(0); }
}
void SetEnabled(bool enabled) {
    if(enabled && !g_ready.load()) return;
    if(enabled && !g_enabled.load()) g_startMs.store(Now());
    g_enabled.store(enabled);
    if(!enabled) g_lift.store(0);
    host::Logf("spinny: %s, nose up 90 deg, yaw 540 deg/s, wheel clearance 0.20m",enabled?"enabled":"disabled");
}
void OnSerialized(void* outgoing,const void* snapshot) {
    g_serialized.fetch_add(1,std::memory_order_relaxed);
    if(!g_enabled.load() || !g_ready.load()) return;
    const auto skip=[&](const char* note) {
        g_note.store(note); g_skipped.fetch_add(1,std::memory_order_relaxed); g_lift.store(0);
    };
    if(!IsReadable(outgoing,math::kStateBytes) || !IsReadable(snapshot,math::kSnapshotBytes)) {
        skip("state unavailable"); return;
    }
    void* raw=LocalBike();
    if(!raw) { skip("waiting for local bike"); return; }
    BikeData bike(raw);
    std::array<double,2> radii{};
    for(int wheel=0;wheel<2;++wheel) {
        const auto tyre=bike.activeTyre(wheel);
        if(!tyre.valid() || !IsReadable(tyre.raw()+off::tyre::kRadius,sizeof(double))) {
            skip("wheel radius unavailable"); return;
        }
        std::memcpy(&radii[wheel],tyre.raw()+off::tyre::kRadius,sizeof(double));
    }
    double lift=0;
    const auto start=g_startMs.load();
    const auto now=Now();
    if(now<start || !math::ChangeState(static_cast<std::byte*>(outgoing),static_cast<const std::byte*>(snapshot),
                                      radii,(now-start)/1000.0,&lift)) {
        skip("invalid pose or wheel geometry"); return;
    }
    g_lift.store(static_cast<float>(lift)); g_note.store("ready");
    g_changed.fetch_add(1,std::memory_order_relaxed);
}
}
