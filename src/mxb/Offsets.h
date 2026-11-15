#pragma once
#include <cstdint>

// Offsets for MX Bikes beta21 (mxbikes.exe, x64, preferred base 0x140000000).
// Everything here is confirmed in the IDB; see NOTES.md for how each one was derived.
// Bump kBuildTag and re-derive if the exe changes.

namespace mxb {
namespace off {

inline constexpr const char* kBuildTag = "beta21";
inline constexpr uintptr_t   kPreferredBase = 0x140000000ull;

// ---- RVAs (add to the real module base) ---------------------------------
// Per-world entity collections. Physics_StepWorld (0x1401BE3A0) indexes this as
//   world = kWorldArray + kWorldStride * (worldId - 1),  worldId in 1..3
// and each world holds six collection pointers; the one at +40 is the bikes,
// because its elements are passed straight into Rider_Update. This is where the
// LIVE bikes are.
inline constexpr uintptr_t kWorldArray     = 0x5677A0;
inline constexpr size_t    kWorldStride    = 24928;
inline constexpr int       kWorldCount     = 3;
inline constexpr size_t    kWorldBikeColl  = 40;

// Registry** for bike .cfg data, used by the setup/garage code. NOT the live
// bikes: it reads back null in a running session, which is what made the first
// attempt at this report "Bikes loaded: 0".
inline constexpr uintptr_t kBikeDataRegistry = 0x396AD8;

// Raw network client state: 0 = never brought up, 1 = idle, 2 = connecting,
// 3/4 = connected. Written only by Net_ClientUpdate and Net_BeginConnect.
inline constexpr uintptr_t kNetConnectionState = 0x3D7900;

// The game's own main window handle, stored by App_CreateWindow. Authoritative,
// unlike WindowFromDC on whatever DC happens to reach the swap hook (the engine
// also makes a throwaway window while probing pixel formats).
inline constexpr uintptr_t kWindowHandle = 0xE4B238;

// The main-camera render function (0x14024EF80). Its glFrustum call at
// 0x14024F17A returns to 0x14024F180, so a glFrustum hook can tell the player
// view apart from the shadow and reflection passes by checking whether the
// return address lands inside this range.
inline constexpr uintptr_t kMainViewRenderBegin = 0x24EF80;
inline constexpr uintptr_t kMainViewRenderEnd   = 0x24F736;

// Camera sets, loaded by Camera_LoadCfg (0x14011AD40) from a bike's cameras.cfg.
// One 1396-byte entry per loaded vehicle, array runs to 0x561308 (50 entries).
//   +0               int   in-use flag
//   +240             int   number of onboard cameras
//   +244  + 60*c     char  name
//   +272  + 60*c     float position[3]
//   +284  + 60*c     float rotation[2]
//   +296  + 60*c     float FOV          <- what the override writes
//   +856             int   number of external cameras
//   +908  + 52*c     float external FOV
inline constexpr uintptr_t kCameraSets       = 0x550260;
inline constexpr uintptr_t kCameraSetsEnd    = 0x561308;
inline constexpr size_t    kCameraSetStride  = 1396;
inline constexpr size_t    kCamOnboardCount  = 240;
inline constexpr size_t    kCamOnboardFov    = 296;
inline constexpr size_t    kCamOnboardStride = 60;
inline constexpr size_t    kCamExternalCount = 856;
inline constexpr size_t    kCamExternalFov   = 908;
inline constexpr size_t    kCamExternalStride = 52;
inline constexpr int       kCamMaxPerSet     = 10;

// Rider_Reset(bike) -- the game's own rider reset, extracted from the full bike
// placement routine at 0x140197060, which calls it with nothing but the bike
// pointer. Clears the crashed flag and rider state and re-seats the rider
// transform from the bike, leaving the bike itself where it is. That is exactly
// what recovering from a crash needs.
inline constexpr uintptr_t kRiderReset = 0x1957D0;

// The game's UI input collectors. App_WndProc calls both for every message,
// before it falls through to DefWindowProcA. No-oping them is how we take input
// away from the game without touching the message pump.
inline constexpr uintptr_t kInputCollectKeyboard = 0x28DFD0;
inline constexpr uintptr_t kInputCollectMouse    = 0x28E1F0;

// Crash-tolerance sites inside Rider_Update.
inline constexpr uintptr_t kCrashLatConst    = 0x329410;  // double 4000.0,  exclusive -> patch in place
inline constexpr uintptr_t kCrashSustConst   = 0x32940C;  // float -2000.0,  exclusive -> patch in place
inline constexpr uintptr_t kCrashVertInsn    = 0x1A9CB2;  // movsd xmm15, [rip+d32] -> 10000.0, SHARED const
inline constexpr uintptr_t kCrashLongInsn    = 0x1A9F6D;  // comiss xmm0, [rip+d32] -> 15000.0, SHARED const

// Displacement field position and instruction length for the two shared-constant sites.
inline constexpr uint32_t kCrashVertDispAt = 5, kCrashVertInsnLen = 9;
inline constexpr uint32_t kCrashLongDispAt = 3, kCrashLongInsnLen = 7;

// ---- View / camera / world render ---------------------------------------
// View_BuildFrustum(view) -- the ONE place a field of view becomes geometry.
// Reads the FOV in degrees from camera+4 (camera = *(void**)(view+48)) and from
// it writes the near-plane extents, the far-plane extents, the screen-space
// projection matrix, the eight frustum corners AND the six culling planes.
//
// This is why the first FOV attempt broke the periphery: rescaling the planes in
// a glFrustum hook widened the projection but left the culling planes at the
// stock FOV, so anything only visible in the newly exposed edges was culled
// away. There are also three glFrustum calls that take the player's view object
// (one in Render_MainView, two in Render_ViewPasses), and a return-address
// filter only ever caught the first.
//
// Non-player views exclude themselves without any call-site filtering:
//   view+192 == 1   reflection recursion, derives planes from the existing ones
//   view+192 == 2   explicit bounds at view+56..+68
//   view+196 != 0   ortho
//   view+188 == 1 && view+400 == 1   Render_CubeProbe's 6 faces (camera+4 = 90)
inline constexpr uintptr_t kViewBuildFrustum = 0x249510;

// Render_VisorEffects(view): draws two 728-byte effect slots, dirt then rain.
// View_Init (0xB120) loads effects/visor/dirt.cfg into slot 0 and rain.cfg into
// slot 1. Render_Frame copies the persistent view to its stack before drawing.
inline constexpr uintptr_t kRenderVisorEffects = 0x252510;

// Applied-value interceptors for the world look. Hooking the setter beats poking
// the global behind it: the value is rewritten as it is applied, so nothing has
// to be saved or restored and it does not matter how often, or from where, the
// game recomputes it.
inline constexpr uintptr_t kGlSetClearColor = 0x22FB50;  // (float r, float g, float b)
inline constexpr uintptr_t kGlSetFogColor   = 0x228CF0;  // (float rgb[3])
inline constexpr uintptr_t kGlSetFogEnable  = 0x228C90;  // (int on)

// The globals those are fed from in Render_Frame / Render_MainView. Recorded for
// diagnostics; the modifier intercepts rather than writing these.
// GL_LIGHT0's AMBIENT, not its diffuse: Render_MainView calls
// glLightfv(0x4000, 0x1200, ...) and 0x1200 is GL_AMBIENT (GL_DIFFUSE is
// 0x1201). With GL_LIGHT_MODEL_AMBIENT left at {0,0,0,1}, LIGHT0's ambient term
// is where the world's light level lives.
inline constexpr uintptr_t kSunLight    = 0x57CC40;  // float[3], GL_LIGHT0 GL_AMBIENT
inline constexpr uintptr_t kClearColor  = 0x57CC50;  // float[3], clear/sky_color
inline constexpr uintptr_t kFogColor    = 0x57CC10;  // float[4]
inline constexpr uintptr_t kFogDensity  = 0x57CB70;  // float, glFogf(GL_FOG_DENSITY)
inline constexpr uintptr_t kFogEnabled  = 0x57CB74;  // int

// Render-target descriptor (a pointer to it). +24 int width, +28 int height,
// +32 float pixel aspect. A view object's viewport fractions are relative to it.
inline constexpr uintptr_t kScreenPtr = 0x584540;

// View object, 0x848 bytes, built on the render stack every frame.
namespace view {
inline constexpr size_t kBuilt        = 16;   // int, dirty flag; View_BuildFrustum sets it to 1
inline constexpr size_t kViewportW    = 20;   // float, fraction of the render target
inline constexpr size_t kViewportH    = 24;
inline constexpr size_t kViewportX    = 28;
inline constexpr size_t kViewportY    = 36;
inline constexpr size_t kCamera       = 48;   // camera object pointer
inline constexpr size_t kLeft         = 72;   // float, near-plane extents
inline constexpr size_t kTop          = 76;
inline constexpr size_t kRight        = 80;
inline constexpr size_t kBottom       = 84;
inline constexpr size_t kFarLeft      = 88;   // float, far-plane extents (+92/+96/+100)
inline constexpr size_t kNear         = 108;  // float
inline constexpr size_t kFar          = 112;  // float
// World -> normalised window coordinates, 16 floats. Push a world point through
// it with the Mat_TransformPoint convention and divide x and y by z: both come
// out as fractions of the render target, x left to right and y BOTTOM to top
// (GL window convention), spanning [vpX, vpX+vpW] and [vpY, vpY+vpH].
inline constexpr size_t kScreenMatrix = 116;
inline constexpr size_t kCorners      = 208;  // 8 float[3] frustum corners
inline constexpr size_t kCullPlanes   = 304;  // 6 planes, 16 bytes each
inline constexpr size_t kMirror       = 188;  // int, 1 for mirrored passes and cube faces
inline constexpr size_t kMode         = 192;  // int, 0 normal / 1 reflection / 2 explicit bounds
inline constexpr size_t kOrtho        = 196;  // int
inline constexpr size_t kProbe        = 400;  // int, 1 for the cube probe faces
}  // namespace view

// Camera object, reached through view+48.
namespace cam {
inline constexpr size_t kFov    = 4;   // float, DEGREES, referenced to 4:3 (see View_BuildFrustum)
inline constexpr size_t kMatrix = 32;  // 16 floats
}  // namespace cam

// ---- Rider aids (profile.ini globals) -----------------------------------
// One contiguous block, parsed by sub_1400ECD00 from profile.ini sections
// [aids] and [hardcore]. The game already ships a self-balancing assist and an
// auto-lean assist, which is what car mode is built on: these are the engine's
// own mechanisms, so nothing has to fight the solver.
//
// They are SOURCES, not live state. sub_14006D050 pushes them into the engine at
// session/rider setup through the dispatcher at kEngineApi, so a change here
// lands on the next session rather than on the next frame.
//
// sub_1400D5FA0 is the settings writer and shows the scales' natural units:
// leanhelp_scale is a UI slider / 100, i.e. nominally 0..1.
inline constexpr uintptr_t kAidAutoShift        = 0xE557F8;  // int
inline constexpr uintptr_t kAidBrakeHelp        = 0xE557FC;  // int
inline constexpr uintptr_t kAidTractionHelp     = 0xE55800;  // int
inline constexpr uintptr_t kAidAutoRiderFbLean  = 0xE55804;  // int
inline constexpr uintptr_t kAidAutoRiderLrLean  = 0xE55808;  // int
inline constexpr uintptr_t kAidAutoRiderDab     = 0xE5580C;  // int
inline constexpr uintptr_t kAidAutoRiderSit     = 0xE55810;  // int
inline constexpr uintptr_t kAidLeanHelp         = 0xE55814;  // int
inline constexpr uintptr_t kAidLeanHelpScale    = 0xE55818;  // float, nominally 0..1
inline constexpr uintptr_t kAidShiftHelp        = 0xE5581C;  // int
inline constexpr uintptr_t kAidAutoClutch       = 0xE55820;  // int
inline constexpr uintptr_t kAidAutoTyreChange   = 0xE55824;  // int
inline constexpr uintptr_t kAidStabilityHelp    = 0xE55828;  // int
inline constexpr uintptr_t kAidStabilityScale   = 0xE5582C;  // float, nominally 0..1
inline constexpr uintptr_t kAidDirectSteer      = 0xE55830;  // int: 0 off, 1 torque, 2 angle
inline constexpr uintptr_t kAidDirectSteerTq    = 0xE55834;  // float
inline constexpr uintptr_t kAidDirectSteerTq2   = 0xE55838;  // float, scaled by a per-bike value
inline constexpr uintptr_t kAidDirectSteerAngle = 0xE5583C;  // float, used by mode 2

// The engine's universal settings bus: (int opcode, int riderId, ...). Opcodes
// seen in sub_14006D050: 613 autoshift, 614 brakehelp, 615 tractionhelp,
// 618 rider auto-lean bitmask, 619 autodab, 620 autosit, 621 leanhelp,
// 622 stabilityhelp, 623 autoclutch, 624 shifthelp, 625 autotyreschange,
// 626 directsteer mode, 627 directsteer max torque, 628 directsteer max angle.
//
// DO NOT CALL IT. It is a runtime-resolved pointer (0xFFFFFFFFFFFFFFFF in the
// file) invoked from 11,169 sites with int, float and char* in the same third
// argument, i.e. variadic. Getting a vararg type wrong here is the same mistake
// as the three-argument Net_QueryConnectionState call that crashed the game on
// the very first inject.
inline constexpr uintptr_t kEngineApi = 0x566C48;

// ---- Model materials / chams -------------------------------------------
// Mat_SetupMaterial(a1, a2, a3, a4, a5) -- the per-mesh material setup, and the
// single chokepoint for recolouring anything the game draws: it holds all 22
// glMaterialfv calls and the only glMaterialf. Five integer arguments, no
// floats, so it is straightforward to hook.
//
// a1 points at the scene entity: the function reads *a1 and then tests
// entity+268 and entity+272 exactly as the render loops do.
//
// It has only two callers, which is what separates the rider from everything
// else without needing to classify meshes:
//   sub_14024E740      the general scene renderer (track, bikes, props)
//   sub_140261220      called ONLY for a type-1 entity's +6392 object
inline constexpr uintptr_t kMaterialSetup = 0x24B6E0;
inline constexpr uintptr_t kRiderRenderBegin = 0x261220;
inline constexpr uintptr_t kRiderRenderEnd   = 0x26191C;  // 0x261220 + 0x6FC

// The scene entity collection, same layout as every other engine collection
// (see Process.h). Entities carry a type at +268: 1 is a vehicle, 0 is track
// and scenery.
inline constexpr uintptr_t kEntityCollection = 0x584870;
namespace entity {
inline constexpr size_t kType   = 268;   // int, 1 = vehicle
inline constexpr size_t kFlags  = 272;   // bit 8 = hidden, bit 10 = ?
inline constexpr size_t kModel  = 480;   // void*, the ordinary model
inline constexpr size_t kRider  = 6392;  // void*, drawn by sub_140261220
}  // namespace entity

// Local player. Profile_ApplyAidsToRider indexes a slot array by the 1-based
// player index and reads the rider id out of slot+4:
//   slot = kPlayerSlots + kPlayerSlotStride * (g_LocalPlayerIndex - 1)
//   riderId = *(int*)(slot + 4)
inline constexpr uintptr_t kLocalPlayerIndex = 0xE62710;  // int, 1-based
inline constexpr uintptr_t kPlayerSlots      = 0xF4EE20;
inline constexpr size_t    kPlayerSlotStride = 23332;
inline constexpr size_t    kPlayerSlotId     = 4;  // int, LOCAL physics rider handle; remote slots may have 0
// Remote join paths 0x5CAE0 / 0x5E570 match and initialize the roster identity
// at +12. Their physics proxy handle lives separately at +20664.
inline constexpr size_t kPlayerSlotRosterId = 12; // int; zero is a valid roster ID
// Remote slots refreshed by 0x16040 before the local send in 0x6E1D0.
inline constexpr size_t kPlayerSlotRemote = 20652;
inline constexpr size_t kPlayerSlotReceiving = 20656;
inline constexpr size_t kPlayerSlotSnapshot = 20676; // 0x760 bytes; first int is validity
inline constexpr int kMaxPlayers = 50;
inline constexpr uintptr_t kGameplayClientState = 0x598904; // 7: sending gameplay state
inline constexpr uintptr_t kHostingSession = 0xE6270C; // nonzero selects hosting path

// ---- Rigid body (the physics library's own) -----------------------------
// Identified from the accessor family around 0x1402AC8B0, each of which does
// nothing but return an offset or write three floats:
//   sub_1402AC8B0 -> body+192   sub_1402AC7A0 writes it   position
//   sub_1402AC8C0 -> body+208                             orientation
//   sub_1402AC8E0 -> body+272   sub_1402AC870 writes it   linear velocity
//   sub_1402AC8F0 -> body+288   sub_1402AC890 writes it   angular velocity
//   sub_1402AC990 adds a world vector to body+304         force accumulator
//   sub_1402AC9D0 adds a world vector to body+320         torque accumulator
//
// Because the setters are plain float writes with no side effects, setting a
// velocity can be replicated with memory writes instead of calling game code.
namespace body {
inline constexpr size_t kPosition = 192;  // float[3]
// 3 rows of 4 floats at +208 / +224 / +240. sub_1402ACAE0 rotates a local vector
// into world using rows (f52,f53,f54), (f56,f57,f58), (f60,f61,f62), which is
// what fixes the stride at 4 floats per row.
inline constexpr size_t kOrientation = 208;
inline constexpr size_t kLinVel      = 272;  // float[3]
inline constexpr size_t kAngVel      = 288;  // float[3], world axes, +Y is yaw
inline constexpr size_t kForceAccum  = 304;  // float[3]
inline constexpr size_t kTorqueAccum = 320;  // float[3]
}  // namespace body

// Control descriptor table: 28 entries, 0x20 stride.
//   +0  const char* name ("CTRL_LEAN", "CTRL_THROTTLE", ...)
//   +8  int type: 1 unipolar axis, 2 bipolar axis, 3/4 button
//   +16 float, +20 float
//   +24 int default DirectInput scancode (negative direction for a bipolar axis)
//   +28 int default scancode, positive direction
//
// Index 0 is CTRL_LEAN, which is this game's name for the steering axis -- type
// 2, defaulting to DIK_LEFT (0xCB) / DIK_RIGHT (0xCD). The default scancodes
// matching the game's real defaults is what confirms the indices.
inline constexpr uintptr_t kControlTable       = 0x395F40;
inline constexpr size_t    kControlStride      = 0x20;
inline constexpr int       kControlCount       = 28;
inline constexpr int       kControlLean        = 0;   // the steering axis
inline constexpr int       kControlThrottle    = 1;

// Rider_Update(bike), the per-frame rider step. Hex-Rays cannot decompile it
// (20,273 bytes, "local variable allocation has failed"), so anything needed
// out of it has to come from the disassembly or from a runtime probe.
inline constexpr uintptr_t kRiderUpdate = 0x1A8480;

// ---- Bike physics object -------------------------------------------------
namespace bike {
// Minimum readable prefix, through the final validation flag at +145892.
// This is not the full allocation size. The checksum reads through +145591.
inline constexpr size_t kSize = 145896;

// Engine
// Live layout confirmed by 0x14018FF80, 0x1401B5B30 and a running bike.
// 0x140168CD0 uses a different layout; its engine/suspension offsets are wrong here.
inline constexpr size_t kEngineId        = 1816;   // char[32]
inline constexpr size_t kEngineInertia   = 1848;   // double
inline constexpr size_t kNumMappings     = 2020;
inline constexpr size_t kMappingBase     = 2024;
inline constexpr size_t kMappingStride   = 264;
inline constexpr size_t kMappingTorque   = 208;    // Curve, relative to a mapping
inline constexpr size_t kMappingBrake    = 232;    // Curve, relative to a mapping
inline constexpr int    kMaxMappings     = 5;
inline constexpr size_t kMappingSetting  = 3344;
inline constexpr size_t kClutchMaxTorque = 3504;   // double
// Aerodynamics, read by Bike_LoadCfg as doubles. CdA is the drag coefficient
// already multiplied by frontal area, so it is the single number that sets top
// speed: drag goes as CdA * v^2 and terminal velocity as 1/sqrt(CdA). Min is
// the tucked pose, Max is sitting up.
inline constexpr size_t kAeroMinCdA      = 145576;  // double, Aero/MinCdA
inline constexpr size_t kAeroMaxCdA      = 145584;  // double, Aero/MaxCdA

// Loader 0x14018FF80 divides RPM by 60: these fields hold revolutions/second.
inline constexpr size_t kEngineMaxRPM    = 3648;   // float
inline constexpr size_t kEngineShiftRPM  = 3652;   // float
inline constexpr size_t kNumLimiter      = 1984;
inline constexpr size_t kLimiter0        = 1992;   // double[NumLimiter], max 3 before mappings

// Gearbox / driveline
inline constexpr size_t kGearingCount    = 3664;
inline constexpr size_t kGearingPairs    = 3668;   // int driving/driven pairs, max 100
inline constexpr size_t kRearSprocketCount = 13528;
inline constexpr size_t kRearSprocketTable = 13532;
inline constexpr size_t kNumGears        = 12512;
inline constexpr size_t kGear0           = 12556;  // int[NumGears]
inline constexpr size_t kUpShiftTime     = 12864;  // float
inline constexpr size_t kDownShiftTime   = 12868;  // float
inline constexpr size_t kDisengageTime   = 12872;  // float
inline constexpr size_t kDisengageMaxTq  = 12876;  // float
inline constexpr size_t kEngageTime      = 12888;  // float
inline constexpr size_t kEngageMaxSpdDif = 12892;  // float
inline constexpr size_t kFirstAxisInert  = 13960;  // double
inline constexpr size_t kSecondAxisInert = 13984;  // double
inline constexpr size_t kDrivelineEff    = 14016;  // double

// Suspension
inline constexpr size_t kFrontMass        = 16672;  // double
inline constexpr size_t kFrontSpring      = 16720;  // SettingRange
inline constexpr size_t kFrontOil         = 19464;  // SettingRange
inline constexpr size_t kFrontPreload     = 19496;  // SettingRange
inline constexpr size_t kFrontRideHeight  = 19516;  // SettingRange
inline constexpr size_t kFrontBumpThr0    = 18384;  // double
inline constexpr size_t kFrontBumpThr1    = 18392;  // double
inline constexpr size_t kFrontReboundThr0 = 19400;  // double
inline constexpr size_t kFrontReboundThr1 = 19408;  // double
inline constexpr size_t kFrontTopOutK     = 19544;  // float

inline constexpr size_t kRearMass         = 20408;  // double
inline constexpr size_t kRearSpring       = 20896;  // SettingRange
inline constexpr size_t kRearPreload      = 23652;  // SettingRange
inline constexpr size_t kRearRideHeight   = 23672;  // SettingRange
inline constexpr size_t kRearRodLength    = 23692;  // SettingRange
inline constexpr size_t kRearBumpThr0     = 22552;  // double
inline constexpr size_t kRearBumpThr1     = 22560;  // double
inline constexpr size_t kRearReboundThr0  = 23568;  // double
inline constexpr size_t kRearReboundThr1  = 23576;  // double
inline constexpr size_t kRearBumpStopK    = 23632;  // float  BumpStop/SpringBase
inline constexpr size_t kRearBumpStopRise = 23636;  // float  BumpStop/SpringRisingRate
inline constexpr size_t kRearBumpStopDamp = 23640;  // float  BumpStop/DampingBase
inline constexpr size_t kRearTopOutK      = 23716;  // float

// Chassis
inline constexpr size_t kChassisMass      = 832;   // double
inline constexpr size_t kFrontLatStiff    = 864;   // double
inline constexpr size_t kRearLatStiff     = 896;   // double

// Tyres (embedded)
inline constexpr size_t kTyreCount0   = 24824;   // wheel 0: how many tyre records were loaded
inline constexpr size_t kTyreArray0   = 24832;   // wheel 0, tyre 0
inline constexpr size_t kWheelStride  = 58128;
inline constexpr size_t kTyreStride   = 4912;
inline constexpr int    kMaxTyres     = 10;
inline constexpr size_t kActiveTyreIdx0 = 73952;   // wheel%d/tyresetting; + kWheelStride for wheel 1
inline constexpr size_t kActiveTyrePtr0 = 73960;   // + kWheelStride for wheel 1  (qword)
inline constexpr size_t kWheelFriction0 = 73968;   // + kWheelStride for wheel 1  (double)

// Rider control inputs, latched by Bike_Update (0x1401ADA40) each step. The raw
// block at +624 is copied into the active block at +720, and when the rider has
// crashed (+142828) or is in the pit (+147496) Bike_Update ZEROES the active
// block -- which is what proves these are the controls, since a crashed rider
// cannot steer.
//
// The order matches g_ControlTable exactly, with the two ints landing precisely
// where CTRL_SHIFTUP and CTRL_SHIFTDOWN sit, and that is what pins the mapping:
//   +624 double  CTRL_LEAN  <- THE STEERING AXIS, -1..+1
//   +632 double  throttle       +640 double  brake
//   +648 double  rear brake     +656 double  clutch
//   +664 int     shift up       +668 int     shift down
//   +672 double  fwd/back lean  +680 double  left/right lean
// Active copies: 720, 728, 736, 744, 752, (780/784/788 gear state), 760, 768.
// Prefer the ACTIVE copy: it is what the physics actually used, and it is
// already zeroed while crashed.
inline constexpr size_t kInputRawSteer       = 624;  // double
inline constexpr size_t kInputActiveSteer    = 720;  // double
inline constexpr size_t kInputActiveThrottle = 728;  // double
inline constexpr size_t kInputActiveBrake    = 736;  // double

// The rigid bodies this bike owns, as pointers. Bike_BuildStateSnapshot
// accumulates the centre of mass over exactly these three, skipping the rider
// when crashed, which is what identifies the set.
inline constexpr size_t kChassisBody = 1072;    // void*
inline constexpr size_t kForkBody    = 14208;   // void*
inline constexpr size_t kRiderBody   = 144512;  // void*

// The bike's transform, 16 floats. Row-major with the translation at float index
// 3/7/11 -- Mat_TransformPoint (0x140266D80) settles that much.
//
// DO NOT read its translation as the bike's position. It is the INVERSE
// transform, the same way camera+32 is (View_BuildFrustum derives camera+32 from
// camera+96 and uses camera+96 to push the frustum corners out into world
// space). So the translation is -R*pos: the right magnitude with the axes
// scrambled, which is exactly what put the china hat at a fixed point far
// outside the map. Use kWheelPos0 instead.
inline constexpr size_t kWorldMatrix  = 928;

// Per-wheel world position, float[3] (x, y, z), + kWheelStride for wheel 1.
// Unambiguously world-space and needing no convention guess, because
// Bike_BuildStateSnapshot takes the midpoint of the two and writes it straight
// into the translation slots (float index 3/7/11) of a local->world frame:
//
//   Mat_Copy64(tmp, bike + 145984);
//   tmp[3]  = (bike[75720] + bike[133848]) * 0.5;   // x
//   tmp[7]  = (bike[75724] + bike[133852]) * 0.5;   // y
//   tmp[11] = (bike[75728] + bike[133856]) * 0.5;   // z
//   invert(tmp, tmp);
//   Mat_TransformPoint(out, tmp, worldCentreOfMass);
//
// 133848 - 75720 == 58128 == kWheelStride, which is what identifies them as the
// per-wheel pair. That midpoint is also the engine's own reference point for a
// bike, so it is the right thing to hang an overlay off. World up is +Y.
inline constexpr size_t kWheelPos0    = 75720;  // float[3]

// Rider
inline constexpr size_t kRiderMass    = 142800;  // double
inline constexpr size_t kCrashedFlag  = 142828;  // int
inline constexpr size_t kFallRef      = 142832;  // float[3]
inline constexpr size_t kRiderState   = 144396;  // int
}  // namespace bike

// ---- Tyre record ---------------------------------------------------------
namespace tyre {
inline constexpr size_t kSize = 4912;

inline constexpr size_t kMass    = 136;  // double
inline constexpr size_t kRadius  = 160;  // double
inline constexpr size_t kWidth   = 176;  // double

inline constexpr size_t kPDx1 = 208;  // float, peak longitudinal friction
inline constexpr size_t kPDx2 = 212;
inline constexpr size_t kPDy1 = 276;  // float, peak lateral friction
inline constexpr size_t kPDy2 = 280;
inline constexpr size_t kPDy3 = 284;

inline constexpr size_t kNumCompounds   = 676;
inline constexpr size_t kCompoundBase   = 680;
inline constexpr size_t kCompoundStride = 688;
inline constexpr int    kMaxCompounds   = 8;

// Per-surface, relative to a compound base: + N + 32*surface
inline constexpr size_t kDryGrip       = 100;
inline constexpr size_t kWetGrip       = 104;
inline constexpr size_t kLongStiffness = 108;
inline constexpr size_t kLatStiffness  = 112;
inline constexpr size_t kResistDry     = 116;
inline constexpr size_t kResistWet     = 120;
inline constexpr int    kNumSurfaces   = 15;

inline constexpr size_t kSpringBase   = 4840;  // double
inline constexpr size_t kSpringFactor = 4848;  // double
inline constexpr size_t kDamper       = 4856;  // double
}  // namespace tyre

}  // namespace off
}  // namespace mxb
