#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "host/Log.h"
#include "host/Hooks.h"
#include "mxb/Spinny.h"
#include "mxb/NetworkRam.h"
#include "mxb/CrashPatch.h"
#include "mxb/Diagnostics.h"
#include "mxb/Modifiers.h"

namespace {
uintptr_t testBase = 0;
mxb::Settings settings;
void Check(bool okay, const char* what) { if (!okay) throw std::runtime_error(what); }
std::string ReadFileText(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
void Store(uintptr_t rva, std::uint32_t value) { std::memcpy(reinterpret_cast<void*>(testBase+rva), &value, 4); }
}

// Supply a private synthetic image and settings instead of the real game.
namespace mxb {
uintptr_t ModuleBase() { return testBase; }
Settings& Get() { return settings; }
Status GetStatus() { Status s; s.bikesSeen=1; s.bikesPatched=1; return s; }
namespace host {
VerificationStatus GetVerificationStatus() { return {true, 0, 2, "invalid_curve_layout", 123, 100}; }
}
namespace spinny {
Status GetStatus() { return {true,true,"ready",10,8,2,0.75f}; }
}
namespace networkram {
Status GetStatus() { return {true,true,"cycling players",12,1,2,20,3,{6,5,4,4,1}}; }
}
namespace crash {
bool IsApplied() { return false; }
bool NoCrashActive() { return false; }
}
}

int main() {
    std::filesystem::path diagnosticPath;
    DWORD originalAttributes = INVALID_FILE_ATTRIBUTES;
    void* memory = nullptr;
    try {
        constexpr size_t imageBytes=0xA00000;
        memory=VirtualAlloc(nullptr,imageBytes,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
        Check(memory!=nullptr,"Could not allocate synthetic image");
        testBase=reinterpret_cast<uintptr_t>(memory);
        auto dos=static_cast<IMAGE_DOS_HEADER*>(memory);
        dos->e_magic=IMAGE_DOS_SIGNATURE; dos->e_lfanew=0x80;
        auto nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(testBase+dos->e_lfanew);
        nt->Signature=IMAGE_NT_SIGNATURE; nt->FileHeader.Machine=IMAGE_FILE_MACHINE_AMD64;
        nt->FileHeader.TimeDateStamp=0x6B032CE7;
        nt->OptionalHeader.Magic=IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        nt->OptionalHeader.SizeOfImage=0x10FD000;
        Store(0x598904,7); Store(0x3D7900,4); Store(0x9CC444,9); Store(0x9CC448,0xFFFFFFFF);
        Store(0x9CBECC,4);
        for (unsigned i=0;i<4;++i) Store(0x9CBED0+i*4,100+i);
        settings.grip.peakLong=1.25f; settings.engine.torque=1.5f;
        wchar_t exe[32768]={}; GetModuleFileNameW(nullptr,exe,32768);
        diagnosticPath=std::filesystem::path(exe).replace_extension(L".ban-diagnostics.log");
        const auto previousSize=std::filesystem::exists(diagnosticPath) ? std::filesystem::file_size(diagnosticPath) : 0;
        mxb::host::LogInit();
        const auto first=static_cast<const unsigned char*>(memory);
        const std::vector<unsigned char> before(first,first+imageBytes);
        mxb::diagnostics::Capture();
        Check(mxb::diagnostics::GetStatus().saved,"Capture did not save");
        Check(std::memcmp(before.data(),memory,imageBytes)==0,"Capture changed game memory");
        const auto text=ReadFileText(diagnosticPath).substr(static_cast<size_t>(previousSize));
        Check(text.find("last_server_event_diagnostic_signed=-1")!=std::string::npos,"Signed reason missing");
        Check(text.find("known_path=bike_setup_validation_failure")!=std::string::npos,"Reason interpretation missing");
        Check(text.find("client_integrity_summary_3=103")!=std::string::npos,"Integrity summary missing");
        Check(text.find("peak_long=1.25")!=std::string::npos,"Grip setting missing");
        Check(text.find("engine torque=1.5")!=std::string::npos,"Engine setting missing");
        Check(text.find("verification_last_failure=invalid_curve_layout")!=std::string::npos,"Stock view failure reason missing");
        Check(text.find("spinny_integrated_ready=1 spinny_enabled=1")!=std::string::npos,"Integrated Spinny state missing");
        Check(text.find("spinny_lift_metres=0.75")!=std::string::npos,"Spinny clearance offset missing");
        Check(text.find("network_ram_ready=1 network_ram_enabled=1 network_ram_changed=12")!=std::string::npos,"Network ram state missing");
        Check(text.find("network_ram_target_slot=2 network_ram_target_id=20 network_ram_targets=3")!=std::string::npos,"Network ram target missing");
        Check(text.find("network_ram_scan_in_use=6 network_ram_scan_remote=5 network_ram_scan_receiving=4")!=std::string::npos,"Network ram scan diagnostics missing");
        Check(text.find("network_ram_build=fast_spin_v3_ylock network_ram_bike_only=0 network_ram_spin_degrees_per_second=2160 network_ram_lead_ms=220")!=std::string::npos,"Network ram spin/prediction missing");
        Check(text.find("network_ram_height=target_snapshot")!=std::string::npos,"Network ram height tracking missing");
        Store(0x9CC448,1);
        mxb::diagnostics::Capture();
        const auto mismatch=ReadFileText(diagnosticPath).substr(static_cast<size_t>(previousSize+text.size()));
        Check(mismatch.find("mismatched_summary_index=0")!=std::string::npos,"First checksum mismatch index missing");
        Check(mismatch.find("mismatched_summary_component=world_plus_local_bike_checksum")!=std::string::npos,
              "First checksum mismatch interpretation missing");
        const auto savedSize=std::filesystem::file_size(diagnosticPath);
        mxb::host::LogInit();
        Check(std::filesystem::file_size(diagnosticPath)==savedSize,"Reload truncated diagnostic history");
        nt->FileHeader.TimeDateStamp=0;
        mxb::diagnostics::Capture();
        Check(mxb::diagnostics::GetStatus().saved,"Partial capture failed to save");
        const auto partial=ReadFileText(diagnosticPath).substr(static_cast<size_t>(savedSize));
        Check(partial.find("network_fields_skipped=unrecognized_or_unreadable_game_headers")!=std::string::npos,
              "Unrecognized build was not rejected");
        Check(partial.find("last_server_event_reason=")==std::string::npos,"Unrecognized build memory was decoded");
        originalAttributes=GetFileAttributesW(diagnosticPath.c_str());
        Check(originalAttributes!=INVALID_FILE_ATTRIBUTES,"Could not read log attributes");
        Check(SetFileAttributesW(diagnosticPath.c_str(),originalAttributes|FILE_ATTRIBUTE_READONLY)!=FALSE,
              "Could not make test destination read-only");
        const unsigned captures=mxb::diagnostics::GetStatus().captures;
        mxb::diagnostics::Capture();
        Check(!mxb::diagnostics::GetStatus().saved,"Write failure was reported as saved");
        Check(mxb::diagnostics::GetStatus().captures==captures,"Failed capture incremented saved counter");
        SetFileAttributesW(diagnosticPath.c_str(),originalAttributes);
        originalAttributes=INVALID_FILE_ATTRIBUTES;
        VirtualFree(memory,0,MEM_RELEASE);
        std::puts("PASS: read-only capture, signed ban code, integrity summary, settings, persistent history, build guard, write failure.");
        return 0;
    } catch(const std::exception& e) {
        if(originalAttributes!=INVALID_FILE_ATTRIBUTES) SetFileAttributesW(diagnosticPath.c_str(),originalAttributes);
        if(memory) VirtualFree(memory,0,MEM_RELEASE);
        std::fprintf(stderr,"FAIL: %s\n",e.what()); return 1;
    }
}
