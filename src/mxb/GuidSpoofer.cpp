#include "GuidSpoofer.h"
#include "Process.h"
#include "host/Log.h"
#include <windows.h>
#include <objbase.h>
#include <mutex>

namespace mxb::guid {
namespace {
std::mutex g_mutex;
Status g_status;
Value g_value{};
}
Status GetStatus() { std::lock_guard<std::mutex> lock(g_mutex); return g_status; }
void SetHookReady(bool ready, const char* note) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_status.ready = ready; g_status.note = note;
    if (!ready) g_status.enabled = false;
}
bool SetEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (enabled && (!g_status.ready || !Valid(g_value))) return false;
    g_status.enabled = enabled;
    host::Logf("guid: %s (applies to subsequent outgoing player-info payloads)", enabled ? "enabled" : "disabled");
    return true;
}
bool SetValue(const char* text) {
    Value value{};
    if (!text || !Parse(text, value)) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_value = value; g_status.replacement = Format(value);
    g_status.note = "GUID selected; reconnect to apply";
    return true;
}
bool Generate() {
    GUID random{};
    if (FAILED(CoCreateGuid(&random))) return false;
    Value value{};
    std::memcpy(value.data(), &random, value.size());
    const auto text = Format(value);
    return SetValue(text.data());
}
RewriteResult RewriteCopy(const void* source, int bytes, Payload& output) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_status.ready || !g_status.enabled) return RewriteResult::PassThrough;
    if (bytes != kPayloadBytes || !IsReadable(source, kPayloadBytes) ||
        !Project(source, kPayloadBytes, g_value, output)) {
        ++g_status.skipped;
        g_status.note = "send blocked: payload unavailable, layout differs, or GUID unchanged";
        host::Logf("guid: player-info send blocked; replacement unavailable or identical");
        return RewriteResult::Blocked;
    }
    ++g_status.changed;
    g_status.note = "outgoing GUID replaced; server acceptance unverified";
    host::Logf("guid: copied player-info payload, GUID replaced, count=%llu", static_cast<unsigned long long>(g_status.changed));
    return RewriteResult::Replaced;
}
}
