#include <windows.h>
#include <cstdio>
#include <stdexcept>
#include "mxb/GuidSpoofer.h"

namespace mxb {
bool IsReadable(const void* pointer, size_t bytes) {
    MEMORY_BASIC_INFORMATION region{};
    return pointer && VirtualQuery(pointer,&region,sizeof(region)) && region.State == MEM_COMMIT &&
        !(region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && reinterpret_cast<uintptr_t>(pointer) + bytes <=
        reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
}
namespace host { void Logf(const char*,...) {} }
}
namespace {
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
}
int main() {
    using namespace mxb::guid;
    try {
        Value value{};
        Check(Parse("0123456789aBcDeF01",value),"Valid 72-bit GUID rejected");
        const Value expected{0x01,0xEF,0xCD,0xAB,0x89,0x67,0x45,0x23,0x01};
        Check(value == expected,"Profile-to-wire byte order wrong");
        Check(std::string_view(Format(value).data()) == "0123456789ABCDEF01","GUID formatting wrong");
        const auto previous = value;
        for (auto text : {"", "0", "000000000000000000", "646464646464646464", "0123456789ABCDEF012", "0x123", "AB-CD", "123G"}) {
            Check(!Parse(text,value),"Invalid GUID accepted");
            Check(value == previous,"Invalid parse changed selected value");
        }
        Check(Parse("F",value) && value[0] == 15 && value[8] == 0,"Short GUID parsing wrong");
        value = previous;
        Payload source{}, output{};
        for (size_t i=0;i<source.size();++i) source[i]=std::byte((i*31+17)&255);
        const auto baseline=source;
        output.fill(std::byte{0xCC}); const auto untouched=output;
        Check(!Project(source.data(),683,value,output) && output == untouched,"Short packet changed output");
        Check(!Project(source.data(),685,value,output) && output == untouched,"Long packet changed output");
        Check(!Project(nullptr,684,value,output),"Null packet accepted");
        Check(!Project(source.data(),684,Value{},output),"Zero replacement accepted");
        Check(Project(source.data(),684,value,output),"Valid packet rejected");
        Check(source == baseline,"Source GUID or packet was modified");
        for (size_t i=0;i<output.size();++i) {
            if (i>=648 && i<657) Check(output[i]==std::byte(expected[i-648]),"Wrong outgoing GUID byte");
            else Check(output[i]==baseline[i],"Unrelated payload/session field changed");
        }
        const auto replaced=output;
        Check(!Project(replaced.data(),684,value,output) && output==replaced,"Identical GUID handled incorrectly");

        Check(SetValue("0123456789ABCDEF01"),"Runtime selection failed");
        Check(!SetEnabled(true),"Enabled without hooks");
        SetHookReady(true,"test");
        Check(RewriteCopy(source.data(),684,output)==RewriteResult::PassThrough,"Disabled spoofer changed payload");
        Check(SetEnabled(true),"Could not enable");
        Check(RewriteCopy(source.data(),684,output)==RewriteResult::Replaced && output==replaced,"Runtime outgoing copy failed");
        Check(source==baseline,"Runtime wrote real client payload");
        Check(!SetValue("nonsense"),"Invalid runtime GUID accepted");
        Check(GetStatus().replacement == Format(expected),"Invalid selection replaced active GUID");
        Check(RewriteCopy(source.data(),1,output)==RewriteResult::Blocked,"Wrong runtime payload size accepted");
        void* inaccessible=VirtualAlloc(nullptr,4096,MEM_RESERVE | MEM_COMMIT,PAGE_NOACCESS);
        Check(inaccessible != nullptr,"Guard allocation failed");
        Check(RewriteCopy(inaccessible,684,output)==RewriteResult::Blocked,"Unreadable source accepted");
        VirtualFree(inaccessible,0,MEM_RELEASE);
        auto status=GetStatus();
        Check(status.changed==1 && status.skipped==2,"Runtime counters wrong");
        SetHookReady(false,"unloaded");
        Check(!GetStatus().enabled && RewriteCopy(source.data(),684,output)==RewriteResult::PassThrough,"Unload left spoofer enabled");
        for (int i=0;i<32;++i) {
            Check(Generate(),"Random GUID generation failed");
            Value generated{};
            Check(Parse(GetStatus().replacement.data(),generated),"Random GUID invalid");
        }
        std::puts("GUID smoke passed: byte order, payload isolation, invalid inputs, guards, toggle, random generation.");
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr,"GUID smoke failed: %s\n",error.what()); return 1; }
}
