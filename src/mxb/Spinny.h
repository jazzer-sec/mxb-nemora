#pragma once
#include <cstdint>

namespace mxb::spinny {
struct Status {
    bool ready=false;
    bool enabled=false;
    const char* note="hook not installed";
    uint64_t serialized=0,changed=0,skipped=0;
    float liftMetres=0;
};
Status GetStatus();
void SetHookReady(bool ready,const char* note);
void SetEnabled(bool enabled);
void OnSerialized(void* outgoing,const void* snapshot);
}
