#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>

// Ported from Spinny's verified Euler/relative-rider transform. All writes are
// confined to a serialized outgoing state; the source snapshot remains const.
namespace mxb::spinny::math {
constexpr size_t kStateBytes = 288;
constexpr size_t kSnapshotBytes = 0x760;
constexpr double kPi = 3.14159265358979323846;
constexpr double kRadians = kPi / 180.0;
constexpr double kPitch = -90.0; // +Z points toward the front wheel; -90 points it up.
constexpr double kYawRate = 540.0; // 1.5 full turns/second around world +Y
constexpr double kClearance = 0.20; // metres above the higher original tyre bottom
using Mat = std::array<double,9>;
using Vec = std::array<double,3>;

inline float Read(const std::byte* p,size_t offset) {
    float value; std::memcpy(&value,p+offset,sizeof(value)); return value;
}
inline void Write(std::byte* p,size_t offset,double value) {
    const float f=static_cast<float>(value); std::memcpy(p+offset,&f,sizeof(f));
}
inline bool Finite(const Vec& v) {
    for(double value:v) if(!std::isfinite(value)) return false;
    return true;
}
inline Mat Multiply(const Mat& a,const Mat& b) {
    Mat result{};
    for(int r=0;r<3;++r) for(int c=0;c<3;++c) for(int k=0;k<3;++k)
        result[r*3+c]+=a[r*3+k]*b[k*3+c];
    return result;
}
inline Mat Transpose(const Mat& m) { return {m[0],m[3],m[6],m[1],m[4],m[7],m[2],m[5],m[8]}; }
inline Vec Rotate(const Mat& m,const Vec& v) {
    Vec result{};
    for(int r=0;r<3;++r) for(int c=0;c<3;++c) result[r]+=m[r*3+c]*v[c];
    return result;
}
inline Mat Euler(double pitch,double yaw,double roll) {
    const double p=pitch*kRadians,y=yaw*kRadians,r=roll*kRadians;
    const double cp=std::cos(p),sp=std::sin(p),cy=std::cos(y),sy=std::sin(y),cr=std::cos(r),sr=std::sin(r);
    return {cy*cr+sy*sp*sr,-cy*sr+sy*sp*cr,sy*cp,
            cp*sr,cp*cr,-sp,-sy*cr+cy*sp*sr,sy*sr+cy*sp*cr,cy*cp};
}
inline Vec Angles(const Mat& m) {
    const double pitch=std::asin(std::clamp(-m[5],-1.0,1.0));
    if(std::abs(std::cos(pitch))>1e-7)
        return {pitch/kRadians,std::atan2(m[2],m[8])/kRadians,std::atan2(m[3],m[4])/kRadians};
    return {pitch/kRadians,std::atan2(-m[6],m[0])/kRadians,0};
}
inline Mat Rotation(const std::byte* snapshot,size_t offset) {
    Mat result{};
    for(int r=0;r<3;++r) for(int c=0;c<3;++c) result[r*3+c]=Read(snapshot,offset+4*(r*4+c));
    return result;
}
inline Vec Position(const std::byte* snapshot,size_t matrix) {
    return {Read(snapshot,matrix+12),Read(snapshot,matrix+28),Read(snapshot,matrix+44)};
}
inline bool ValidRotation(const Mat& m) {
    for(double value:m) if(!std::isfinite(value)) return false;
    const Mat product=Multiply(m,Transpose(m));
    for(int i=0;i<9;++i) if(std::abs(product[i]-(i%4==0?1.0:0.0))>0.03) return false;
    const double det=m[0]*(m[4]*m[8]-m[5]*m[7])-m[1]*(m[3]*m[8]-m[5]*m[6])+m[2]*(m[3]*m[7]-m[4]*m[6]);
    return std::abs(det-1.0)<0.03;
}

inline bool ChangeState(std::byte* state,const std::byte* snapshot,
                        const std::array<double,2>& radii,double seconds,double* appliedLift=nullptr) {
    if(!state || !snapshot || !std::isfinite(seconds) || seconds<0) return false;
    for(double radius:radii) if(!std::isfinite(radius) || radius<0.05 || radius>2.0) return false;
    const Mat oldBike=Rotation(snapshot,4),oldRider=Rotation(snapshot,0x438);
    if(!ValidRotation(oldBike) || !ValidRotation(oldRider)) return false;
    // Reduce time before multiplying, including long-running sessions.
    const double yaw=std::fmod(seconds,360.0/kYawRate)*kYawRate;
    const Mat newBike=Euler(kPitch,yaw,0);
    const Mat delta=Multiply(newBike,Transpose(oldBike));
    const Vec riderAngles=Angles(Multiply(delta,oldRider));
    const Vec bikePos{Read(state,0),Read(state,4),Read(state,8)};
    Vec riderRelative{};
    for(int axis=0;axis<3;++axis) riderRelative[axis]=Read(state,264+axis*4)-bikePos[axis];
    if(!Finite(bikePos) || !Finite(riderRelative)) return false;
    const Vec riderRelativeNew=Rotate(delta,riderRelative);
    double ground=-std::numeric_limits<double>::infinity();
    double lowest=std::numeric_limits<double>::infinity();
    // Wheel transforms are copied by Bike_BuildStateSnapshot at +252/+452.
    // Radii come from the local bike's active tyres (+160 double). Snapshot
    // +324/+524 are tyre profile dimensions, NOT wheel radii.
    for(int wheel=0;wheel<2;++wheel) {
        const Vec centre=Position(snapshot,252+200*wheel);
        if(!Finite(centre)) return false;
        Vec relative{};
        for(int axis=0;axis<3;++axis) relative[axis]=centre[axis]-bikePos[axis];
        double distanceSquared=0;
        for(double value:relative) distanceSquared+=value*value;
        if(distanceSquared>100) return false; // stale geometry or a mismatched bike
        ground=std::max(ground,centre[1]-radii[wheel]);
        const Vec turned=Rotate(delta,relative);
        lowest=std::min(lowest,bikePos[1]+turned[1]-radii[wheel]);
    }
    const double lift=std::max(0.0,ground+kClearance-lowest);
    if(!std::isfinite(lift) || lift>20) return false;
    // Commit after validation. No failure may leave a partially rewritten packet.
    Write(state,4,bikePos[1]+lift);
    Write(state,24,kPitch); Write(state,28,yaw); Write(state,32,0);
    Write(state,36,0);
    Write(state,40,std::cos(kPitch*kRadians)*kYawRate*kRadians);
    Write(state,44,-std::sin(kPitch*kRadians)*kYawRate*kRadians);
    for(int axis=0;axis<3;++axis) {
        Write(state,264+axis*4,bikePos[axis]+riderRelativeNew[axis]+(axis==1?lift:0));
        Write(state,276+axis*4,riderAngles[axis]);
    }
    if(appliedLift) *appliedLift=lift;
    return true;
}
}
