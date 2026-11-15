#include "Diagnostics.h"

#include <windows.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <sstream>

#include "CrashPatch.h"
#include "Modifiers.h"
#include "Process.h"
#include "host/Log.h"
#include "host/Hooks.h"
#include "Spinny.h"
#include "NetworkRam.h"

namespace mxb {
namespace diagnostics {
namespace {
std::mutex g_lock;
Status g_status;

// Headers from the beta21 executable used to derive these diagnostic RVAs.
constexpr DWORD kTimeDateStamp = 0x6A21833D;
constexpr DWORD kImageSize = 0x10FD000;

template <typename T>
bool Read(uintptr_t address, T& value) {
    SIZE_T bytes = 0;
    return address && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
                                        &value, sizeof(value), &bytes) && bytes == sizeof(value);
}

bool KnownHeaders(uintptr_t base) {
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    return Read(base, dos) && dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0 &&
           dos.e_lfanew < 0x100000 && Read(base + dos.e_lfanew, nt) &&
           nt.Signature == IMAGE_NT_SIGNATURE && nt.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
           nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
           nt.FileHeader.TimeDateStamp == kTimeDateStamp && nt.OptionalHeader.SizeOfImage == kImageSize;
}
}

void Capture() {
    std::lock_guard<std::mutex> guard(g_lock);
    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    char timestamp[40] = {};
    std::snprintf(timestamp, sizeof(timestamp), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
                  utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond, utc.wMilliseconds);
    std::ostringstream report;
    report << "\n=== MXB CLIENT BAN DIAGNOSTIC " << timestamp << " ===\n"
           << "pid=" << GetCurrentProcessId() << " testing_no_guards=" << MXB_TESTING_NO_GUARDS << '\n';
    const uintptr_t base = ModuleBase();
    const bool known = KnownHeaders(base);
    report << "module_base=0x" << std::hex << base << std::dec
           << " known_beta21_headers=" << known << '\n';

    bool complete = known;
    if (known) {
        auto field = [&](const char* name, uintptr_t rva) {
            std::uint32_t value = 0;
            const bool valid = Read(base + rva, value);
            complete = complete && valid;
            report << name << '=';
            if (valid) report << value; else report << "unreadable";
            report << '\n';
            return std::pair<bool, std::uint32_t>{valid, value};
        };
        field("connection_state", 0x598904);
        field("master_connection_state", 0x3D7900);
        field("disconnect_category", 0x9CBE38);
        field("disconnect_reason", 0x9CBE3C);
        field("disconnect_detail", 0x9CBE40);
        const auto reason = field("last_server_event_reason", 0x9CC444);
        const auto detail = field("last_server_event_diagnostic", 0x9CC448);
        if (detail.first) {
            const auto signedDetail = static_cast<std::int64_t>(detail.second) -
                (detail.second & 0x80000000U ? 0x100000000LL : 0);
            report << "last_server_event_diagnostic_signed=" << signedDetail << '\n';
            if (reason.first && reason.second == 9 && signedDetail == -1)
                report << "known_path=bike_setup_validation_failure\n";
            else if (reason.first && reason.second == 9 && signedDetail == -2)
                report << "known_path=reported_value_comparison_failure_meaning_unresolved\n";
            else if (reason.first && reason.second == 9 && signedDetail == 0)
                report << "known_path=integrity_summary_count_mismatch\n";
            else if (reason.first && reason.second == 9 && signedDetail >= 1 && signedDetail <= 8) {
                report << "known_path=integrity_summary_value_mismatch\n"
                       << "mismatched_summary_index=" << signedDetail - 1 << '\n';
                if (signedDetail == 1) report << "mismatched_summary_component=world_plus_local_bike_checksum\n";
            }
        }
        const auto count = field("client_integrity_summary_count", 0x9CBECC);
        if (count.first && count.second <= 8) {
            for (std::uint32_t i = 0; i < count.second; ++i) {
                char label[48] = {};
                std::snprintf(label, sizeof(label), "client_integrity_summary_%u", i);
                field(label, 0x9CBED0 + i * 4);
            }
        } else if (count.first) {
            report << "summary_values_skipped=invalid_count\n";
            complete = false;
        }
    } else {
        report << "network_fields_skipped=unrecognized_or_unreadable_game_headers\n";
    }

    const Settings& s = Get();
    const mxb::Status mod = mxb::GetStatus();
    const auto verification = host::GetVerificationStatus();
    const auto spin=spinny::GetStatus();
    report << "spinny_integrated_ready=" << spin.ready << " spinny_enabled=" << spin.enabled
           << " spinny_yaw_degrees_per_second=540 spinny_nose_up_degrees=90 spinny_clearance_metres=0.20\n"
           << "spinny_serialized=" << spin.serialized << " spinny_changed=" << spin.changed
           << " spinny_skipped=" << spin.skipped << " spinny_lift_metres=" << spin.liftMetres << '\n'
           << "spinny_status=" << spin.note << '\n';
    const auto ram = networkram::GetStatus();
    report << "network_ram_ready=" << ram.ready << " network_ram_enabled=" << ram.enabled
           << " network_ram_changed=" << ram.changed << " network_ram_skipped=" << ram.skipped << '\n'
           << "network_ram_target_slot=" << ram.targetSlot << " network_ram_target_id=" << ram.targetId
           << " network_ram_targets=" << ram.targets << " network_ram_status=" << ram.note << '\n';
    report << "network_ram_scan_in_use=" << ram.scan.inUse << " network_ram_scan_remote=" << ram.scan.remote
           << " network_ram_scan_receiving=" << ram.scan.receiving << " network_ram_scan_snapshots=" << ram.scan.snapshots
           << " network_ram_scan_invalid_pose=" << ram.scan.invalidPose << '\n';
    report << "network_ram_build=fast_spin_v3_ylock network_ram_bike_only=" << ram.bikeOnly
           << " network_ram_spin_degrees_per_second=2160 network_ram_lead_ms=" << ram.leadMilliseconds << '\n'
           << "network_ram_outgoing_yaw=" << ram.outgoingYaw << " network_ram_outgoing_yaw_rate=" << ram.outgoingYawRate
           << " network_ram_height=target_snapshot\n";
    report << "verification_build=live_layout_2\n"
           << "verification_ready=" << verification.ready << " stock_replies=" << verification.stockReplies
           << " stock_unavailable=" << verification.unavailable << '\n'
           << "verification_last_failure=" << verification.lastFailure << '\n'
           << "verification_last_actual_checksum=" << verification.lastActualChecksum
           << " verification_last_stock_checksum=" << verification.lastStockChecksum << '\n';
    report << "master_enabled=" << s.masterEnabled << " writes_blocked=" << mod.writesBlocked
           << " bikes_seen=" << mod.bikesSeen << " bikes_patched=" << mod.bikesPatched << '\n'
           << "enabled grip=" << s.grip.enabled << " suspension=" << s.suspension.enabled
           << " engine=" << s.engine.enabled << " transmission=" << s.transmission.enabled
           << " crash=" << s.crash.enabled << '\n'
           << "grip peak_long=" << s.grip.peakLong << " peak_lat=" << s.grip.peakLat
           << " dry=" << s.grip.surfaceDry << " wet=" << s.grip.surfaceWet << '\n'
           << "engine torque=" << s.engine.torque << " braking=" << s.engine.engineBraking
           << " clutch_torque=" << s.engine.clutchTorque << '\n'
           << "suspension front_spring=" << s.suspension.frontSpring << " rear_spring=" << s.suspension.rearSpring
           << " front_damping=" << s.suspension.frontDamping << " rear_damping=" << s.suspension.rearDamping << '\n'
           << "transmission gear_ratio=" << s.transmission.gearRatio << " final_drive=" << s.transmission.finalDrive
           << " shift_speed=" << s.transmission.shiftSpeed << " clutch_speed=" << s.transmission.clutchSpeed << '\n'
           << "crash tolerance=" << s.crash.tolerance << " no_crash=" << s.crash.noCrash
           << " auto_recover=" << s.crash.autoRecover << " patch_applied=" << crash::IsApplied()
           << " no_crash_active=" << crash::NoCrashActive() << '\n'
           << "note=Client values may be stale and are not an atomic snapshot. Capture immediately after rejection.\n"
           << "note=Reason 9 is Cheating; this capture does not prove which server check failed.\n"
           << "=== END MXB CLIENT BAN DIAGNOSTIC ===\n";
    char path[MAX_PATH] = {};
    g_status.saved = host::LogDiagnostic(report.str().c_str(), path, sizeof(path));
    if (g_status.saved) {
        ++g_status.captures;
        std::snprintf(g_status.message, sizeof(g_status.message), "Capture %u saved%s.",
                      g_status.captures, complete ? "" : " (partial)");
        host::Logf("ban diagnostic capture %u saved to %s", g_status.captures, path);
    } else {
        std::snprintf(g_status.message, sizeof(g_status.message), "Diagnostic log could not be saved (Win32 %lu).", GetLastError());
    }
}

Status GetStatus() {
    std::lock_guard<std::mutex> guard(g_lock);
    return g_status;
}

}  // namespace diagnostics
}  // namespace mxb
