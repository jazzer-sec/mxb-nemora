#include "CrashPatch.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

#include "Offsets.h"
#include "Process.h"

namespace mxb {
namespace crash {
namespace {

// Stock thresholds, straight out of Rider_Update.
constexpr double kStockLat  = 4000.0;    // fabs(sumLat)  > 4000.0   (double, exclusive const)
constexpr float  kStockSust = -2000.0f;  // avg25         < -2000.0  (float,  exclusive const)
constexpr double kStockVert = 10000.0;   // fabs(sumVert) > 10000.0  (double, SHARED const)
constexpr float  kStockLong = 15000.0f;  // sumLong       > 15000.0  (float,  SHARED const)

// Expected opcode bytes at the two shared-constant sites, everything before the
// displacement. If these do not match we are on a different build and must not patch.
constexpr uint8_t kVertOpcode[] = {0xF2, 0x44, 0x0F, 0x10, 0x3D};  // movsd  xmm15, [rip+d32]
constexpr uint8_t kLongOpcode[] = {0x0F, 0x2F, 0x05};              // comiss xmm0,  [rip+d32]

// Our own copies of the two shared constants, so the instructions can point here
// instead of at .rdata that other code also reads. Page-pinned for the process
// lifetime; the game only ever reads them.
#pragma pack(push, 8)
struct Relocated {
    double vert;
    float  longF;
    float  _pad;
};
#pragma pack(pop)
Relocated g_relocated{kStockVert, kStockLong, 0.0f};

// The conditional jumps that SKIP a Rider_Detach call. Turning each into an
// unconditional jump makes that exit unreachable. rel8 displacement is left
// alone; only the opcode changes.
struct SkipJump {
    uintptr_t rva;
    uint8_t   cond;    // expected opcode
    uint8_t   uncond;  // 0xEB, jmp rel8
    const char* what;
};
constexpr SkipJump kSkipJumps[] = {
    {off::kCrashLeanJump, 0x76, 0xEB, "lean angle (jbe)"},
};

// The ground-contact exit at 0x1401A85BF is deliberately NOT in that list.
//
// It looked like a third crash rule, but it is not one. Rider_GroundQuery fills a
// surface-contact record at bike+144652 and the test is "the rider's reference
// point is at or below that surface". Coming off is the only response the solver
// models for a rider inside the terrain -- there is no attached-and-intersecting
// state. Forcing the skip leaves the rider embedded in the ground and whatever
// runs next ejects it, which showed up as the bike in the air with the camera
// under the surface after a soft landing over a berm, followed by a NaN.
//
// So body-to-dirt contact still ends the run even with Never crash on. Covering
// it properly means auto-recovering after the detach rather than preventing it.

bool g_noCrash = false;

bool    g_applied = false;
bool    g_verified = false;
bool    g_checked = false;

// Saved originals for restore.
double  g_origLat = kStockLat;
float   g_origSust = kStockSust;
int32_t g_origVertDisp = 0;
int32_t g_origLongDisp = 0;

template <typename T>
bool WriteProtected(void* dst, const T& value) {
    DWORD old = 0;
    if (!VirtualProtect(dst, sizeof(T), PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(dst, &value, sizeof(T));
    VirtualProtect(dst, sizeof(T), old, &old);
    FlushInstructionCache(GetCurrentProcess(), dst, sizeof(T));
    return true;
}

// Rewrites the 4-byte RIP-relative displacement of one instruction so it reads
// `target` instead of its original operand.
bool RepointDisp(uintptr_t insn, uint32_t dispAt, uint32_t insnLen, const void* target,
                 int32_t* savedDisp) {
    auto* disp = reinterpret_cast<int32_t*>(insn + dispAt);
    if (savedDisp) *savedDisp = *disp;

    const uintptr_t next = insn + insnLen;
    const int64_t   rel  = static_cast<int64_t>(reinterpret_cast<uintptr_t>(target)) -
                           static_cast<int64_t>(next);
    if (rel > INT32_MAX || rel < INT32_MIN) return false;  // too far for rip-relative

    return WriteProtected(disp, static_cast<int32_t>(rel));
}

bool OpcodeMatches(uintptr_t insn, const uint8_t* bytes, size_t n) {
    if (!IsReadable(reinterpret_cast<void*>(insn), n)) return false;
    return std::memcmp(reinterpret_cast<void*>(insn), bytes, n) == 0;
}

// Checks every site looks like the build we mapped, before we write anything.
bool VerifySites() {
    if (g_checked) return g_verified;
    g_checked = true;
    g_verified = false;

    const uintptr_t latAddr  = Rva(off::kCrashLatConst);
    const uintptr_t sustAddr = Rva(off::kCrashSustConst);
    const uintptr_t vertInsn = Rva(off::kCrashVertInsn);
    const uintptr_t longInsn = Rva(off::kCrashLongInsn);
    if (!latAddr || !sustAddr || !vertInsn || !longInsn) return false;

    if (!IsReadable(reinterpret_cast<void*>(latAddr), sizeof(double))) return false;
    if (!IsReadable(reinterpret_cast<void*>(sustAddr), sizeof(float))) return false;
    if (*reinterpret_cast<double*>(latAddr) != kStockLat) return false;
    if (*reinterpret_cast<float*>(sustAddr) != kStockSust) return false;

    if (!OpcodeMatches(vertInsn, kVertOpcode, sizeof(kVertOpcode))) return false;
    if (!OpcodeMatches(longInsn, kLongOpcode, sizeof(kLongOpcode))) return false;

    // The displacements must still resolve to the stock constants.
    const auto vertTarget = vertInsn + off::kCrashVertInsnLen +
                            *reinterpret_cast<int32_t*>(vertInsn + off::kCrashVertDispAt);
    const auto longTarget = longInsn + off::kCrashLongInsnLen +
                            *reinterpret_cast<int32_t*>(longInsn + off::kCrashLongDispAt);
    if (!IsReadable(reinterpret_cast<void*>(vertTarget), sizeof(double))) return false;
    if (!IsReadable(reinterpret_cast<void*>(longTarget), sizeof(float))) return false;
    if (*reinterpret_cast<double*>(vertTarget) != kStockVert) return false;
    if (*reinterpret_cast<float*>(longTarget) != kStockLong) return false;

    g_verified = true;
    return true;
}

}  // namespace

bool NoCrashActive() { return g_noCrash; }

bool SetNoCrash(bool enabled) {
    if (enabled == g_noCrash) return true;

    // Verify every site before touching any of them, so a build we do not
    // recognise leaves the game completely alone.
    for (const SkipJump& j : kSkipJumps) {
        const uintptr_t at = Rva(j.rva);
        if (!at || !IsReadable(reinterpret_cast<void*>(at), 1)) return false;
        const uint8_t cur = *reinterpret_cast<const uint8_t*>(at);
        if (cur != (enabled ? j.cond : j.uncond)) return false;
    }

    for (const SkipJump& j : kSkipJumps) {
        const uintptr_t at = Rva(j.rva);
        if (!WriteProtected(reinterpret_cast<void*>(at), enabled ? j.uncond : j.cond))
            return false;
    }

    g_noCrash = enabled;
    return true;
}

bool SitesVerified() { return g_verified; }
bool IsApplied() { return g_applied; }

bool Apply(float tolerance) {
    if (!VerifySites()) return false;
    if (!(tolerance > 0.01f)) tolerance = 0.01f;  // also rejects NaN

    // Always scale from the stock values, so repeated calls never compound.
    g_relocated.vert  = kStockVert * tolerance;
    g_relocated.longF = kStockLong * tolerance;

    const uintptr_t latAddr  = Rva(off::kCrashLatConst);
    const uintptr_t sustAddr = Rva(off::kCrashSustConst);
    const uintptr_t vertInsn = Rva(off::kCrashVertInsn);
    const uintptr_t longInsn = Rva(off::kCrashLongInsn);

    if (!g_applied) {
        g_origLat  = *reinterpret_cast<double*>(latAddr);
        g_origSust = *reinterpret_cast<float*>(sustAddr);

        // Do the two displacement rewrites first, because they are the only part
        // that can legitimately fail: our constants live in this DLL, and if the
        // loader placed it more than 2GB from the game module the rip-relative
        // operand cannot reach them. Failing here leaves the game untouched,
        // whereas failing after the in-place writes would leave it half patched
        // with g_applied still false, so Restore would not clean up.
        if (!RepointDisp(vertInsn, off::kCrashVertDispAt, off::kCrashVertInsnLen,
                         &g_relocated.vert, &g_origVertDisp))
            return false;
        if (!RepointDisp(longInsn, off::kCrashLongDispAt, off::kCrashLongInsnLen,
                         &g_relocated.longF, &g_origLongDisp)) {
            // Put the first one back so we leave nothing behind.
            WriteProtected(reinterpret_cast<void*>(vertInsn + off::kCrashVertDispAt),
                           g_origVertDisp);
            return false;
        }
        // From here on every site is ours, so Restore can undo all of it.
        g_applied = true;
    }

    WriteProtected(reinterpret_cast<void*>(latAddr), kStockLat * tolerance);
    // This one is negative, so a larger tolerance has to push it further down.
    WriteProtected(reinterpret_cast<void*>(sustAddr), kStockSust * tolerance);
    return true;
}

void Restore() {
    SetNoCrash(false);
    if (!g_applied) return;

    WriteProtected(reinterpret_cast<void*>(Rva(off::kCrashLatConst)), g_origLat);
    WriteProtected(reinterpret_cast<void*>(Rva(off::kCrashSustConst)), g_origSust);
    WriteProtected(reinterpret_cast<void*>(Rva(off::kCrashVertInsn) + off::kCrashVertDispAt),
                   g_origVertDisp);
    WriteProtected(reinterpret_cast<void*>(Rva(off::kCrashLongInsn) + off::kCrashLongDispAt),
                   g_origLongDisp);

    g_applied = false;
}

}  // namespace crash
}  // namespace mxb
