#include <windows.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <vector>
#include "mxb/Spinny.h"
#include "mxb/SpinnyMath.h"
#include "mxb/Process.h"

namespace {
namespace sm=mxb::spinny::math;
using Packet=std::array<std::byte,sm::kStateBytes>;
using Snapshot=std::array<std::byte,sm::kSnapshotBytes>;
std::vector<uint8_t> live(mxb::off::bike::kSize);
bool localPresent=true;
void Check(bool condition,const char* message) { if(!condition) throw std::runtime_error(message); }
void Near(double actual,double expected,double tolerance=0.0002) {
    if(!std::isfinite(actual) || std::abs(actual-expected)>tolerance) throw std::runtime_error("Numeric mismatch");
}
void MatrixNear(const sm::Mat& a,const sm::Mat& b) { for(int i=0;i<9;++i) Near(a[i],b[i]); }
void PutMatrix(Snapshot& snapshot,size_t offset,const sm::Mat& rotation,const sm::Vec& position) {
    for(int r=0;r<3;++r) {
        for(int c=0;c<3;++c) sm::Write(snapshot.data(),offset+4*(r*4+c),rotation[r*3+c]);
        sm::Write(snapshot.data(),offset+4*(r*4+3),position[r]);
    }
    sm::Write(snapshot.data(),offset+60,1);
}
const sm::Vec origin{120.25,10.5,-315.75};
const std::array<double,2> radii{0.35,0.32};
const std::array<sm::Vec,2> wheelLocal{{{0,-0.40,0.80},{0,-0.42,-0.75}}};
const sm::Vec riderLocal{0.07,0.95,-0.13};
void Fixture(Snapshot& snapshot,Packet& packet,const sm::Mat& oldBike) {
    for(size_t i=0;i<packet.size();++i) packet[i]=std::byte((i*17+3)&255);
    const auto riderRotation=sm::Multiply(oldBike,sm::Euler(-12,5,8));
    const auto riderOffset=sm::Rotate(oldBike,riderLocal);
    sm::Vec riderPosition{};
    for(int axis=0;axis<3;++axis) {
        sm::Write(packet.data(),4*axis,origin[axis]);
        riderPosition[axis]=origin[axis]+riderOffset[axis];
        sm::Write(packet.data(),264+4*axis,riderPosition[axis]);
    }
    PutMatrix(snapshot,4,oldBike,origin);
    PutMatrix(snapshot,0x438,riderRotation,riderPosition);
    for(int wheel=0;wheel<2;++wheel) {
        auto offset=sm::Rotate(oldBike,wheelLocal[wheel]);
        for(int axis=0;axis<3;++axis) offset[axis]+=origin[axis];
        PutMatrix(snapshot,252+200*wheel,sm::Multiply(oldBike,sm::Euler(0,0,50*wheel)),offset);
        // Small profile dimensions must not be mistaken for wheel radii.
        sm::Write(snapshot.data(),324+200*wheel,0.05);
    }
}
void OnlyPoseBytesChanged(const Packet& before,const Packet& after) {
    for(size_t i=0;i<before.size();++i)
        if(!(i>=4 && i<8) && !(i>=24 && i<48) && !(i>=264 && i<288))
            Check(before[i]==after[i],"Unrelated network field changed");
}
}

namespace mxb {
void* LocalBike() { return localPresent?live.data():nullptr; }
bool IsReadable(const void* pointer,size_t bytes) {
    MEMORY_BASIC_INFORMATION region{};
    return pointer && VirtualQuery(pointer,&region,sizeof(region)) && region.State==MEM_COMMIT &&
        !(region.Protect&(PAGE_NOACCESS|PAGE_GUARD)) && reinterpret_cast<uintptr_t>(pointer)+bytes<=
        reinterpret_cast<uintptr_t>(region.BaseAddress)+region.RegionSize;
}
namespace host { void Logf(const char*,...) {} }
}

int main() {
    try {
        unsigned cases=0;
        for(double pitch:{-75.0,0.0,45.0,89.0}) for(double roll:{-45.0,0.0,70.0})
            for(double oldYaw:{0.0,73.0,179.0}) for(double phase:{0.0,0.03125,0.0625,0.125,0.1875,20.21875}) {
                Snapshot snapshot{}; Packet packet{};
                const auto oldBike=sm::Euler(pitch,oldYaw,roll);
                Fixture(snapshot,packet,oldBike);
                const auto source=snapshot; const auto before=packet;
                double lift=-1;
                Check(sm::ChangeState(packet.data(),snapshot.data(),radii,phase,&lift),"Valid pose skipped");
                Near(sm::Read(packet.data(),24),-90);
                Near(sm::Read(packet.data(),28),std::fmod(phase,2.0/3.0)*540);
                const auto newBike=sm::Euler(sm::Read(packet.data(),24),sm::Read(packet.data(),28),sm::Read(packet.data(),32));
                const auto forward=sm::Rotate(newBike,{0,0,1});
                Near(forward[0],0); Near(forward[1],1); Near(forward[2],0);
                const auto worldRate=sm::Rotate(newBike,{sm::Read(packet.data(),36),sm::Read(packet.data(),40),sm::Read(packet.data(),44)});
                Near(worldRate[0],0); Near(worldRate[1],540*sm::kRadians); Near(worldRate[2],0);
                const sm::Vec riderAngles{sm::Read(packet.data(),276),sm::Read(packet.data(),280),sm::Read(packet.data(),284)};
                MatrixNear(sm::Multiply(sm::Transpose(newBike),sm::Euler(riderAngles[0],riderAngles[1],riderAngles[2])),sm::Euler(-12,5,8));
                const auto expectedRider=sm::Rotate(newBike,riderLocal);
                for(int axis=0;axis<3;++axis)
                    Near(sm::Read(packet.data(),264+4*axis),origin[axis]+expectedRider[axis]+(axis==1?lift:0));
                double reference=-1e20,lowest=1e20;
                for(int wheel=0;wheel<2;++wheel) {
                    const auto old=sm::Position(snapshot.data(),252+200*wheel);
                    reference=std::max(reference,old[1]-radii[wheel]);
                    const auto turned=sm::Rotate(newBike,wheelLocal[wheel]);
                    lowest=std::min(lowest,origin[1]+lift+turned[1]-radii[wheel]);
                }
                Check(lowest+0.0002>=reference+0.20,"Lower tyre intersects reference ground");
                Check(lift>=0,"Offset lowered the bike");
                Near(sm::Read(packet.data(),4),origin[1]+lift);
                Check(source==snapshot,"Source/local snapshot changed");
                OnlyPoseBytesChanged(before,packet);
                ++cases;
            }
        Snapshot snapshot{}; Packet packet{};
        Fixture(snapshot,packet,sm::Euler(0,0,0));
        const auto before=packet;
        auto badRadii=radii; badRadii[1]=std::numeric_limits<double>::quiet_NaN();
        Check(!sm::ChangeState(packet.data(),snapshot.data(),badRadii,0.1) && packet==before,"Invalid radius partially changed packet");
        Check(!sm::ChangeState(packet.data(),snapshot.data(),radii,std::numeric_limits<double>::infinity()) && packet==before,
              "Invalid time partially changed packet");
        auto invalid=snapshot; sm::Write(invalid.data(),4,std::numeric_limits<float>::quiet_NaN());
        Check(!sm::ChangeState(packet.data(),invalid.data(),radii,0.1) && packet==before,"Invalid rotation changed packet");
        invalid=snapshot; sm::Write(invalid.data(),252+12,10000);
        Check(!sm::ChangeState(packet.data(),invalid.data(),radii,0.1) && packet==before,"Mismatched wheel geometry accepted");

        mxb::BikeData bike(live.data());
        for(int wheel=0;wheel<2;++wheel) {
            auto tyre=bike.tyreSlot(wheel,0);
            std::memcpy(tyre.raw()+mxb::off::tyre::kRadius,&radii[wheel],8);
            auto* pointer=tyre.raw();
            std::memcpy(live.data()+mxb::off::bike::kActiveTyrePtr0+mxb::off::bike::kWheelStride*wheel,&pointer,sizeof(pointer));
        }
        const auto liveBefore=live;
        mxb::spinny::SetHookReady(false,"unavailable"); mxb::spinny::SetEnabled(true);
        Check(!mxb::spinny::GetStatus().enabled,"Enabled with no hook");
        mxb::spinny::SetHookReady(true,"ready");
        mxb::spinny::OnSerialized(packet.data(),snapshot.data());
        Check(packet==before,"Disabled mode changed output");
        mxb::spinny::SetEnabled(true);
        mxb::spinny::OnSerialized(packet.data(),snapshot.data());
        Check(packet!=before && mxb::spinny::GetStatus().changed==1,"Enabled integration did not transform");
        Check(mxb::spinny::GetStatus().liftMetres>0,"Lift status missing");
        Check(live==liveBefore,"Controller wrote live bike");
        mxb::spinny::SetEnabled(false); packet=before;
        mxb::spinny::OnSerialized(packet.data(),snapshot.data());
        Check(packet==before,"Disable did not return to pass-through");
        localPresent=false; mxb::spinny::SetEnabled(true);
        mxb::spinny::OnSerialized(packet.data(),snapshot.data());
        Check(packet==before && mxb::spinny::GetStatus().skipped==1,"Missing local bike did not skip");
        localPresent=true;
        mxb::spinny::SetHookReady(false,"unloaded");
        Check(!mxb::spinny::GetStatus().enabled,"Unload left effect enabled");
        std::printf("PASS: %u upright poses, horizontal spin/world angular velocity, both tyre bounds +20cm, rider alignment, packet boundaries, local-state preservation, toggle/unload and invalid-data guards.\n",cases);
        return 0;
    } catch(const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what()); return 1; }
}
