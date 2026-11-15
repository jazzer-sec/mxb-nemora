#include "CleanLens.h"

#include <atomic>

namespace mxb::cleanlens {
namespace {
std::atomic<bool> g_ready{false}, g_enabled{false};
std::atomic<uint64_t> g_hidden{0};
}

Status GetStatus() {
    return {g_ready.load(std::memory_order_acquire),
            g_enabled.load(std::memory_order_acquire),
            g_hidden.load(std::memory_order_relaxed)};
}

void SetHookReady(bool ready) {
    if (!ready) g_enabled.store(false, std::memory_order_release);
    g_ready.store(ready, std::memory_order_release);
}

bool SetEnabled(bool enabled) {
    if (enabled && !g_ready.load(std::memory_order_acquire)) return false;
    g_enabled.store(enabled, std::memory_order_release);
    return true;
}

bool ShouldSkipDraw() {
    if (!g_ready.load(std::memory_order_acquire) ||
        !g_enabled.load(std::memory_order_acquire)) return false;
    g_hidden.fetch_add(1, std::memory_order_relaxed);
    return true;
}

}  // namespace mxb::cleanlens
