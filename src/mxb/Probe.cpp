#include "Probe.h"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

#include "BikeData.h"
#include "Offsets.h"
#include "Process.h"
#include "host/Log.h"

namespace mxb {
namespace {

constexpr int kSlots = 3;

std::vector<uint8_t> g_snap[kSlots];
const void*          g_bike[kSlots] = {nullptr, nullptr, nullptr};
ProbeStatus          g_status;

// A loose predicate can match thousands of fields, and a log that long is no
// more use than no log at all.
constexpr int kMaxReported = 70;

float Fl(const std::vector<uint8_t>& b, size_t o) {
    float v;
    std::memcpy(&v, b.data() + o, sizeof(v));
    return v;
}

double Db(const std::vector<uint8_t>& b, size_t o) {
    double v;
    std::memcpy(&v, b.data() + o, sizeof(v));
    return v;
}

bool SaneF(float v, float lim) { return std::isfinite(v) && std::fabs(v) <= lim; }
bool SaneD(double v, double lim) { return std::isfinite(v) && std::fabs(v) <= lim; }

// Opposite sign with both ends clear of zero, which is what separates a real
// axis from a value that merely happens to straddle zero as noise.
bool Flipped(double a, double b, double minMag) {
    if (std::fabs(a) < minMag || std::fabs(b) < minMag) return false;
    return (a < 0.0) != (b < 0.0);
}

const char* kSlotName[kSlots] = {"A", "B", "C"};

}  // namespace

bool ProbeCapture(int slot) {
    if (slot < 0 || slot >= kSlots) return false;

    auto bikes = LoadedBikes();
    if (bikes.empty()) {
        host::Logf("probe: capture %s failed, no bike loaded", kSlotName[slot]);
        return false;
    }

    uint8_t* raw = bikes.front().raw();
    if (!IsReadable(raw, off::bike::kSize)) {
        host::Logf("probe: capture %s failed, bike %p not fully readable", kSlotName[slot],
                   static_cast<void*>(raw));
        return false;
    }

    g_snap[slot].resize(off::bike::kSize);
    std::memcpy(g_snap[slot].data(), raw, off::bike::kSize);
    g_bike[slot] = raw;
    g_status.have[slot] = true;
    g_status.bike = raw;
    g_status.matches = -1;

    host::Logf("probe: captured %s from bike %p (%zu bytes)", kSlotName[slot],
               static_cast<void*>(raw), off::bike::kSize);
    return true;
}

void ProbeClear() {
    for (int i = 0; i < kSlots; ++i) {
        g_snap[i].clear();
        g_bike[i] = nullptr;
    }
    g_status = ProbeStatus{};
    host::Logf("probe: cleared");
}

int ProbeCompare(int mode) {
    if (mode < 0 || mode >= kProbeModeCount) return 0;

    const bool needsC = (mode == kProbeAxis3F);
    if (!g_status.have[0] || !g_status.have[1] || (needsC && !g_status.have[2])) {
        host::Logf("probe: compare needs captures A and B%s", needsC ? " and C" : "");
        g_status.matches = -1;
        return 0;
    }

    // Comparing two different bikes would report every field that differs
    // between two vehicles, which is all of them.
    g_status.mismatch = (g_bike[0] != g_bike[1]) || (needsC && g_bike[2] != g_bike[0]);
    if (g_status.mismatch) {
        host::Logf("probe: compare refused, captures are from different bikes (%p / %p / %p)",
                   g_bike[0], g_bike[1], g_bike[2]);
        g_status.matches = -1;
        return 0;
    }

    const std::vector<uint8_t>& A = g_snap[0];
    const std::vector<uint8_t>& B = g_snap[1];
    const std::vector<uint8_t>& C = g_snap[2];

    int matches = 0;
    int reported = 0;

    // Fields in this object are naturally aligned, so stepping by the field size
    // rather than by one byte is both correct and what keeps the result list
    // short enough to read.
    const size_t step = (mode == kProbeSignFlipD || mode == kProbeChangedD) ? 8u : 4u;
    const size_t limit = off::bike::kSize - step;

    host::Logf("probe: compare mode='%s' bike=%p", kProbeModes[mode], g_bike[0]);

    for (size_t o = 0; o <= limit; o += step) {
        bool hit = false;
        char detail[128];
        detail[0] = 0;

        switch (mode) {
            case kProbeAxisF:
            case kProbeAxis3F: {
                const float a = Fl(A, o), b = Fl(B, o);
                if (!SaneF(a, 1.5f) || !SaneF(b, 1.5f)) break;
                if (!Flipped(a, b, 0.30)) break;
                if (mode == kProbeAxis3F) {
                    const float c = Fl(C, o);
                    if (!SaneF(c, 1.5f) || std::fabs(c) > 0.10f) break;
                    std::snprintf(detail, sizeof(detail), "A=%+.4f C=%+.4f B=%+.4f", a, c, b);
                } else {
                    std::snprintf(detail, sizeof(detail), "A=%+.4f B=%+.4f", a, b);
                }
                hit = true;
                break;
            }
            case kProbeSignFlipF: {
                const float a = Fl(A, o), b = Fl(B, o);
                if (!SaneF(a, 1.0e4f) || !SaneF(b, 1.0e4f)) break;
                if (!Flipped(a, b, 0.05)) break;
                std::snprintf(detail, sizeof(detail), "A=%+.4f B=%+.4f", a, b);
                hit = true;
                break;
            }
            case kProbeSignFlipD: {
                const double a = Db(A, o), b = Db(B, o);
                if (!SaneD(a, 1.0e4) || !SaneD(b, 1.0e4)) break;
                if (!Flipped(a, b, 0.05)) break;
                std::snprintf(detail, sizeof(detail), "A=%+.5f B=%+.5f", a, b);
                hit = true;
                break;
            }
            case kProbeChangedF: {
                const float a = Fl(A, o), b = Fl(B, o);
                if (!SaneF(a, 1.0e6f) || !SaneF(b, 1.0e6f)) break;
                const float d = std::fabs(b - a);
                if (d < 0.05f || d < 0.05f * std::fabs(a)) break;
                std::snprintf(detail, sizeof(detail), "A=%+.4f B=%+.4f d=%+.4f", a, b, b - a);
                hit = true;
                break;
            }
            case kProbeChangedD: {
                const double a = Db(A, o), b = Db(B, o);
                if (!SaneD(a, 1.0e6) || !SaneD(b, 1.0e6)) break;
                const double d = std::fabs(b - a);
                if (d < 0.05 || d < 0.05 * std::fabs(a)) break;
                std::snprintf(detail, sizeof(detail), "A=%+.5f B=%+.5f d=%+.5f", a, b, b - a);
                hit = true;
                break;
            }
            case kProbeUnchangedF: {
                const float a = Fl(A, o), b = Fl(B, o);
                if (!SaneF(a, 1.0e6f) || a == 0.0f) break;
                if (a != b) break;
                std::snprintf(detail, sizeof(detail), "A=B=%+.4f", a);
                hit = true;
                break;
            }
            default:
                break;
        }

        if (!hit) continue;
        ++matches;
        if (reported < kMaxReported) {
            ++reported;
            host::Logf("probe:   +%zu (0x%zX)  %s", o, o, detail);
        }
    }

    if (matches > reported)
        host::Logf("probe: %d matches, first %d logged -- tighten the capture conditions",
                   matches, reported);
    else
        host::Logf("probe: %d matches", matches);

    g_status.matches = matches;
    g_status.reported = reported;
    return matches;
}

ProbeStatus ProbeState() { return g_status; }

}  // namespace mxb
