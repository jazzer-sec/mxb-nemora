#include <windows.h>
#include <array>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <vector>
#include "mxb/NetworkRam.h"
#include "mxb/NetworkRamMath.h"
#include "mxb/Process.h"

namespace {
namespace nr = mxb::networkram;
namespace pose = nr::pose;
using Packet = std::array<std::byte,pose::kStateBytes>;
using Snapshot = std::array<std::byte,pose::kSnapshotBytes>;
std::vector<uint8_t> image(mxb::off::kPlayerSlots + mxb::off::kMaxPlayers*mxb::off::kPlayerSlotStride);
void Check(bool value,const char* note) { if (!value) throw std::runtime_error(note); }
void Near(double value,double expected) { Check(std::isfinite(value) && std::abs(value-expected)<0.001,"numeric mismatch"); }
void PutInt(uint8_t* bytes,size_t offset,int value) { std::memcpy(bytes+offset,&value,4); }
uint8_t* Slot(int index) { return image.data()+mxb::off::kPlayerSlots+(index-1)*mxb::off::kPlayerSlotStride; }
Packet Fixture() {
    Packet packet{};
    for (size_t i=0;i<packet.size();++i) packet[i]=std::byte((i*17+7)&255);
    for (int axis=0;axis<3;++axis) {
        pose::Write(packet.data(),axis*4,100+axis);
        pose::Write(packet.data(),264+axis*4,100+axis+(axis==1 ? 0.9 : 0.1));
        pose::Write(packet.data(),276+axis*4,0);
    }
    const int32_t attached=0;
    std::memcpy(packet.data()+260,&attached,sizeof(attached));
    return packet;
}
Snapshot Source(const pose::Mat& bike) {
    Snapshot snapshot{};
    const auto rider=pose::Multiply(bike,pose::Euler(-12,5,8));
    for (int row=0;row<3;++row) for (int col=0;col<3;++col) {
        pose::Write(snapshot.data(),4+row*16+col*4,bike[row*3+col]);
        pose::Write(snapshot.data(),0x438+row*16+col*4,rider[row*3+col]);
    }
    return snapshot;
}
void Player(int index,int id,double x,double z) {
    auto* slot=Slot(index);
    PutInt(slot,0,1);
    // Remote join paths initialize +12 and leave the LOCAL physics handle at
    // +4 empty. Requiring that handle was the original all-targets-rejected bug.
    PutInt(slot,mxb::off::kPlayerSlotId,0);
    PutInt(slot,mxb::off::kPlayerSlotRosterId,id);
    PutInt(slot,mxb::off::kPlayerSlotRemote,1);
    PutInt(slot,mxb::off::kPlayerSlotReceiving,1);
    PutInt(slot,mxb::off::kPlayerSlotSnapshot,1);
    auto* snapshot=reinterpret_cast<std::byte*>(slot+mxb::off::kPlayerSlotSnapshot);
    const pose::Vec position{x,3,z};
    for (int row=0;row<3;++row) {
        for (int col=0;col<3;++col) pose::Write(snapshot,4+row*16+col*4,row==col ? 1 : 0);
        pose::Write(snapshot,4+row*16+12,position[row]);
        pose::Write(snapshot,68+row*4,0);
    }
}
}
namespace mxb {
uintptr_t ModuleBase() { return reinterpret_cast<uintptr_t>(image.data()); }
bool IsReadable(const void* pointer,size_t bytes) {
    MEMORY_BASIC_INFORMATION region{};
    return pointer && VirtualQuery(pointer,&region,sizeof(region)) && region.State==MEM_COMMIT &&
        !(region.Protect&(PAGE_NOACCESS|PAGE_GUARD)) &&
        reinterpret_cast<uintptr_t>(pointer)+bytes<=reinterpret_cast<uintptr_t>(region.BaseAddress)+region.RegionSize;
}
}
int main() {
    try {
        const auto original=Fixture();
        const auto oldBike=pose::Euler(12,37,-10);
        const auto localSnapshot=Source(oldBike);
        // Different headings, sweep/spin phases, prediction, and a receiver
        // detached-rider flag with byte-exact original rider coordinates.
        for (double angle : {0.0,0.7,2.3,4.2}) for (uint64_t ms : {0,20,40,79})
            for (double spinTime : {0.0,0.015,0.071,0.125,2.5}) for (bool bikeOnly : {false,true}) {
            nr::Target target{2,20,{12,7,-90},{2,25,-3},{std::sin(angle),0,std::cos(angle)}};
            auto packet=original;
            Check(nr::Project(packet.data(),localSnapshot.data(),target,ms,spinTime,0.12,bikeOnly),"valid projection rejected");
            const double offset=3-ms*0.075;
            const auto newBike=pose::Euler(pose::Read(packet.data(),24),pose::Read(packet.data(),28),pose::Read(packet.data(),32));
            const auto forward=pose::Rotate(newBike,{0,0,1});
            const double spinAngle=spinTime*2160*pose::kRadians;
            Near(forward[0]*target.direction[0]+forward[2]*target.direction[2],-std::sin(spinAngle));
            Near(forward[0]*target.direction[2]-forward[2]*target.direction[0],std::cos(spinAngle));
            Near(forward[1],0);
            for (int axis=0;axis<3;++axis) {
                Near(pose::Read(packet.data(),axis*4),target.position[axis]+(axis==1 ? 0 : target.velocity[axis]*0.12)+target.direction[axis]*offset);
                Near(pose::Read(packet.data(),12+axis*4),target.velocity[axis]-target.direction[axis]*75);
                Near(pose::Read(packet.data(),36+axis*4),axis==1 ? 10 : 0);
            }
            int detached=0; std::memcpy(&detached,packet.data()+260,4);
            Check(detached==(bikeOnly ? 1 : 0),"incorrect rider mode for receiver spin");
            for (size_t i=0;i<packet.size();++i)
                if (i>=48 && !(i>=260 && i<264))
                    Check(packet[i]==original[i],"rider pose or unrelated network field changed");
        }
        nr::Target target{2,20,{1,2,3},{0,0,0},{0,0,1}};
        auto packet=original;
        Check(!nr::Project(packet.data(),localSnapshot.data(),target,80) && packet==original,"expired sweep modified packet");
        target.position[0]=std::numeric_limits<double>::quiet_NaN();
        Check(!nr::Project(packet.data(),localSnapshot.data(),target,0) && packet==original,"NaN target modified packet");
        target.position={1,2,3}; target.velocity={0,0,1000};
        Check(!nr::Project(packet.data(),localSnapshot.data(),target,0) && packet==original,"invalid velocity partially modified packet");
        target.velocity={0,0,0}; target.direction={0,0,0};
        Check(!nr::Project(packet.data(),localSnapshot.data(),target,0) && packet==original,"zero direction accepted");
        target.direction={0,0,1};
        auto invalidSnapshot=localSnapshot;
        pose::Write(invalidSnapshot.data(),4,std::numeric_limits<float>::quiet_NaN());
        Check(!nr::Project(packet.data(),invalidSnapshot.data(),target,0) && packet==original,"invalid source rotation modified packet");
        Check(!nr::Project(packet.data(),nullptr,target,0) && packet==original,"missing source rotation accepted");
        Check(!nr::Project(packet.data(),localSnapshot.data(),target,0,-1) && packet==original,"negative spin time accepted");
        Check(!nr::Project(packet.data(),localSnapshot.data(),target,0,0,0.51) && packet==original,"excessive lead time accepted");
        Check(!nr::Project(packet.data(),localSnapshot.data(),target,0,0,std::numeric_limits<double>::quiet_NaN()) && packet==original,"NaN lead accepted");
        target.velocity={100,0,100};
        Check(nr::Project(packet.data(),localSnapshot.data(),target,0,0,0.25),"clamped prediction rejected");
        Near(std::hypot(pose::Read(packet.data(),0)-1,pose::Read(packet.data(),8)-6),12);
        packet=original;
        Check(nr::Project(packet.data(),localSnapshot.data(),target,0,0,0),"zero prediction rejected");
        Near(pose::Read(packet.data(),0),1); Near(pose::Read(packet.data(),8),6);
        packet=original;
        target.velocity={0,0,0};
        for (double height : {-20.5,0.0,180.25}) for (double verticalSpeed : {-30.0,30.0}) {
            target.position[1]=height; target.velocity[1]=verticalSpeed;
            for (double lead : {0.0,0.22,0.5}) {
                auto heightPacket=original;
                Check(nr::Project(heightPacket.data(),localSnapshot.data(),target,40,0.02,lead),"height tracking projection failed");
                Near(pose::Read(heightPacket.data(),4),height);
                Near(pose::Read(heightPacket.data(),16),verticalSpeed);
            }
        }
        target.position={1,2,3}; target.velocity={0,0,0};
        auto firstSpin=original, nextSpin=original;
        Check(nr::Project(firstSpin.data(),localSnapshot.data(),target,0,0,0) &&
              nr::Project(nextSpin.data(),localSnapshot.data(),target,16,1.0/60.0,0),"spin sample projection failed");
        Near(std::remainder(pose::Read(nextSpin.data(),28)-pose::Read(firstSpin.data(),28),360.0),36);
        auto crashed=original;
        const int32_t detached=1;
        std::memcpy(crashed.data()+260,&detached,4);
        Check(nr::Project(crashed.data(),localSnapshot.data(),target,0),"existing crashed state rejected");
        int32_t preserved=0; std::memcpy(&preserved,crashed.data()+260,4);
        Check(preserved==1,"fast-spin mode changed actual rider crash state");

        nr::Cycle cycle;
        std::vector<nr::Target> targets{{5,50},{2,20},{9,90}};
        Check(cycle.Select(targets,1000)->slot==2,"cycle did not start at first slot");
        Check(cycle.Select(targets,1079)->slot==2,"cycle changed target too early");
        Check(cycle.Select(targets,1080)->slot==5,"cycle did not advance at 80 ms");
        Check(cycle.Select(targets,1160)->slot==9,"cycle skipped player");
        Check(cycle.Select(targets,1240)->slot==2,"cycle did not wrap");
        targets.erase(targets.begin()+1);
        Check(cycle.Select(targets,1241)->slot==5,"departed player remained targeted");
        targets[0].id=51;
        Check(cycle.Select(targets,1242)->slot==9,"reused slot kept stale identity");
        targets.clear(); Check(!cycle.Select(targets,1243) && cycle.slot==0,"empty session did not reset");
        targets.push_back({50,500});
        Check(cycle.Select(targets,3000)->slot==50 && cycle.Select(targets,3080)->slot==50 && cycle.started==3080,"single-player cycle failed");
        cycle.Select(targets,10); Check(cycle.started==10,"clock reversal did not reset sweep");

        PutInt(image.data(),mxb::off::kGameplayClientState,7);
        PutInt(image.data(),mxb::off::kLocalPlayerIndex,1);
        Player(1,10,1,1); Player(2,0,20,40); Player(50,500,500,900);
        // Loaded but unreceived/spectating slots are never targets.
        Player(3,30,30,30); PutInt(Slot(3),mxb::off::kPlayerSlotReceiving,0);
        Player(4,40,40,40);
        pose::Write(reinterpret_cast<std::byte*>(Slot(4)+mxb::off::kPlayerSlotSnapshot),4,
                    std::numeric_limits<float>::quiet_NaN());
        Player(5,50,50,50);
        pose::Write(reinterpret_cast<std::byte*>(Slot(5)+mxb::off::kPlayerSlotSnapshot),68,1000);
        Player(6,60,60,60); PutInt(Slot(6),mxb::off::kPlayerSlotRemote,0);
        auto snapshot=localSnapshot; const auto source=snapshot; const auto gameBefore=image;
        Check(!nr::GetStatus().enabled,"started enabled");
        nr::SetEnabled(true); Check(!nr::GetStatus().enabled,"enabled without hook");
        nr::SetHookReady(true,"test");
        Near(nr::GetStatus().leadMilliseconds,220);
        Check(!nr::GetStatus().bikeOnly,"default mode suppresses receiver angular velocity");
        nr::SetLeadMilliseconds(600); Near(nr::GetStatus().leadMilliseconds,500);
        nr::SetLeadMilliseconds(-1); Near(nr::GetStatus().leadMilliseconds,0);
        nr::SetLeadMilliseconds(std::numeric_limits<float>::quiet_NaN()); Near(nr::GetStatus().leadMilliseconds,0);
        nr::SetLeadMilliseconds(220);
        nr::OnSerialized(packet.data(),snapshot.data()); Check(packet==original,"disabled controller modified packet");
        nr::SetEnabled(true); nr::OnSerialized(packet.data(),snapshot.data());
        auto status=nr::GetStatus();
        Check(status.changed==1 && status.targetSlot==2 && status.targetId==0 && status.targets==2,
              "remote slots with no local handle or zero roster identity rejected");
        Check(status.scan.inUse==6 && status.scan.remote==5 && status.scan.receiving==4 &&
              status.scan.snapshots==4 && status.scan.invalidPose==2,"target scan diagnostics wrong");
        Near(pose::Read(packet.data(),0),20); Near(pose::Read(packet.data(),8),43);
        Near(pose::Read(packet.data(),20),-75); Near(pose::Read(packet.data(),28),90);
        Check(image==gameBefore && snapshot==source,"local game state or source snapshot changed");
        Near(status.outgoingYaw,90); Near(status.outgoingYawRate,10);
        nr::SetBikeOnly(true); packet=original;
        nr::OnSerialized(packet.data(),snapshot.data());
        std::memcpy(&preserved,packet.data()+260,4);
        Check(preserved==1 && nr::GetStatus().bikeOnly,"bike-only option not applied");
        Check(std::memcmp(packet.data()+264,original.data()+264,24)==0,"bike-only option moved original rider pose");
        nr::SetBikeOnly(false);
        PutInt(Slot(2),0,0); packet=original;
        nr::OnSerialized(packet.data(),snapshot.data());
        Check(nr::GetStatus().targetSlot==50,"disconnect did not immediately advance to last slot");
        PutInt(Slot(50),mxb::off::kPlayerSlotSnapshot,0); packet=original;
        nr::OnSerialized(packet.data(),snapshot.data());
        Check(packet==original && nr::GetStatus().targetSlot==0,"no targets changed outgoing state");
        PutInt(Slot(50),mxb::off::kPlayerSlotSnapshot,1);
        PutInt(image.data(),mxb::off::kHostingSession,1);
        nr::OnSerialized(packet.data(),snapshot.data()); Check(packet==original,"hosting path modified packet");
        PutInt(image.data(),mxb::off::kHostingSession,0);
        PutInt(image.data(),mxb::off::kGameplayClientState,0);
        nr::OnSerialized(packet.data(),snapshot.data()); Check(packet==original,"disconnected client modified packet");
        PutInt(image.data(),mxb::off::kGameplayClientState,7);
        nr::OnSerialized(nullptr,snapshot.data());
        nr::SetEnabled(false); nr::OnSerialized(packet.data(),snapshot.data()); Check(packet==original,"disable did not restore pass-through");
        nr::SetEnabled(true); nr::SetHookReady(false,"unloaded");
        Check(!nr::GetStatus().enabled,"unload left feature enabled");
        std::puts("Network ram smoke passed: fast-spin/default and bike-only modes, sampled yaw changes, wire rate bounds, prediction, packet isolation, validation, cycling, session guards, toggle/unload.");
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr,"Network ram smoke failed: %s\n",error.what()); return 1; }
}
