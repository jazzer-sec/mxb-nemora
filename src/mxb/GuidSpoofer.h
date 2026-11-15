#pragma once
#include "GuidPayload.h"

namespace mxb::guid {
struct Status {
    bool ready = false, enabled = false;
    std::array<char, 19> replacement{};
    const char* note = "hook not installed";
    uint64_t changed = 0, skipped = 0;
};
Status GetStatus();
void SetHookReady(bool ready, const char* note);
bool SetEnabled(bool enabled);
bool SetValue(const char* text);
bool Generate();
enum class RewriteResult { PassThrough, Replaced, Blocked };
RewriteResult RewriteCopy(const void* source, int bytes, Payload& output);
}
