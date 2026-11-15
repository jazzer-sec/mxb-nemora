# MX Bikes physics module — working notes

The original scope was singleplayer only. The user later authorized stock bike
verification replies for their private-server testing; see the README's
"Stock bike verification replies" section for the current behavior and limits.

## Network ram

Uses the existing `Net_SerializeLocalBikeState` hook at `0x140015B60`, scoped to
the local gameplay send return address `0x140071A37`. Successful calls produce
288 bytes: world position at +0, world linear velocity at +12, orientation at
+24, local angular velocity at +36, absolute rider position at +264. The feature
changes +0..+47 and, only with Bike only enabled, the detached-rider int at
+260..+263. Rider position and
orientation at +264..+287 are preserved byte for byte. All other bytes and the const source
snapshot remain untouched. It takes precedence over Spinny at this call site.

Target slots are the established 50-entry table at RVA `0xF4EE20`, stride 23332.
`0x140016040` requires slot+0 (in use), +20652 (remote), +20656 (receiving), then
refreshes the 0x760-byte snapshot at +20676. Its first int becomes 1 after a
successful remote decode; missing state clears it. Snapshot root matrix is +4,
translation +16/+32/+48, world velocity +68/+72/+76. Local slot index is excluded.
The reader tracks roster identity, validity, a proper rotation and finite bounded motion.

The first build mistakenly required slot+4 to be positive. That is the local
physics rider handle; remote join paths `0x14005CAE0` and `0x14005E570` do not
initialize it. They match/initialize roster identity at slot+12 and use a
separate remote proxy at +20664. The corrected reader uses +12 (including ID 0),
and follows the engine's nonzero remote/receiving flag checks. The regression
fixture leaves +4 zero for every remote and gives the first target roster ID 0.
Scan counts in the menu and F7 distinguish absent players, missing received
snapshots and invalid poses instead of reporting one generic empty-target error.

Round robin holds each slot/ID for 80 ms, translating the outgoing root from
three metres in front to three metres behind along the target's horizontal +Z
axis. Velocity is the target's velocity minus 75 m/s in that direction. Prediction
adds target world velocity times the selected 0–500 ms interval (220 ms default),
capped to twelve metres horizontally. Outgoing Y is the target's current snapshot
Y exactly on every send; vertical velocity is preserved but does not offset the
snapshot's height. It does not measure network delay. The outgoing
bike uses target heading +90 degrees plus a continuous 2160 degrees/second yaw
phase. Spin time is shared across targets and resets on disable. The published
local Y angular velocity is capped to +10 rad/s to match the beta21 compressor;
the requested pose rotation is faster than the receiver's angular extrapolation.
Source rotations must be valid before any packet write. A disappearing/reused slot advances early;
no active targets, client state other than 7 (RVA 0x598904), or hosting
(RVA 0xE6270C) produces pass-through output.

Receive evidence: `0x14006E1D0` decodes via `0x1400154A0`, then engine API 643
calls `0x140168040` to update a remote physics proxy. That path copies received
position/velocity and uses `Body_SetLinVel` and body placement on initialization
or state reset. This supports testing collision behavior but does not establish
confirmed hits, interpolation behavior, or server acceptance. Those need two
clients. Tests cover packet isolation, bike/rider separation, cycling, slot
reuse/removal, the last slot, client/hosting guards and unchanged source state.

Bike-only mode is optional and off by default. It uses the serialized crash/detached int at +260. Decoder
`0x1400154A0` stores it at snapshot+1072 and copies the absolute rider transform
into snapshot+1080 only when nonzero. Keeping the original 24 rider bytes while
setting this flag separates the remote rider from the moved bike. No local
crash flag is written. Tests verify the exact unchanged rider bytes, detached
flag, yaw phase/rate, prediction distance cap, controls and the original guards.

Spin failure evidence: remote proxy update `0x140168040` copies source+136 to
the proxy bike's crashed flag, and explicitly zeroes proxy+96/+100/+104 angular
velocity when that source field is nonzero. The previous build always forced
the detached flag, preventing receiver-side angular extrapolation. Fast-spin
mode preserves the source flag instead, so attached riders retain angular
velocity. Genuine crashed rider state remains intact. The user explicitly
selected fast spin with the remote rider allowed to follow the bike.

Wire limit evidence: `Net_BuildBikeStateSchema` (0x14AE0) configures type 9
for root Euler/velocity with angle step .05, angular min -12, max +10 and step
.001. Descriptor builder `0x15CB10` case 9 stores those bounds; compressor
`0x15ECC0` case 8 clamps the angular floats. Faster requested yaw samples are
still sent as angles, but displayed receiver spin and collision outcomes need
live validation. F7 records the `fast_spin_v3_ylock` marker, mode, outgoing yaw and
published angular rate. The regression test samples two successive poses and
checks a 36-degree yaw change at a 1/60-second interval.

Nametags were traced through profile `hide_names` at RVA 0xE557EC to HUD
`0x1400F7800` case 4. Its rider loop averages the wheel transforms at snapshot
+252 and +452, adds two metres of height, then updates the label with engine
API 206. The hide-names switch controls the receiving client's labels through
API 208. Tag placement follows the bike, so rider separation cannot hide the
sender's tag. This build makes no anonymous-hit claim and does not change names
or label settings.

### Multiple-bike/reset investigation (November 2)

The inspected reset path does not clone an independent network bike. Engine API
602 calls `Bike_PlaceOnTrack` (0x197060), which resolves the existing object with
0x1BF1A0, clears/repositions its existing bodies, calls `Rider_Reset`, increments
the reset-generation byte at bike+60, and returns. APIs 603/604 call 0x198250,
which also resolves that same bike and ultimately calls `Bike_PlaceOnTrack`.
Local physics creation (API 567, 0x1939A0) and remote proxy creation (API 642,
0x1662D0) are separate from this reset path and from network entity registration.

Network allocation is engine API 813 / 0x298600: it requires the server-world
state at RVA 0xA1E2C8, allocates an 86440-byte server entity record, assigns its
entity ID and owner, and broadcasts packet 24. Server receiver 0x29F1E0, packet
40, looks up each submitted entity through 0x29A540 and requires entity.owner
to match the sending client. It updates an existing owned entity; it does not
allocate a new one. The local send queue (0x2A1E10) also overwrites pending
state for the same entity key, so queuing many poses does not create many bikes.

No client-only simultaneous clone spawner was added or claimed. Replicated
copies need server-created entity registrations, and these reset/state paths do
not supply them on an arbitrary joined server. The requested current-Y behavior
was implemented separately in the existing single-bike transform and tested for
rising/falling targets across prediction settings. Remote interpolation remains
outside this sender's control.

## Clean lens

`View_Init` at `0x14000B120` loads `effects/visor/dirt.cfg` with engine API
171 into effect slot 0 and `effects/visor/rain.cfg` into slot 1. API 174
(`0x140242760`) sets each slot's active flag at +20; API 175
(`0x140242990`) clears it. Slots are 728 bytes, starting at view+416.

`Render_VisorEffects` at `0x140252510` loops over both slots and checks their
type and active flag before drawing either the base texture, splats, or the
type-2 composite. Its sole code caller is `Render_Frame` at `0x140254C4D`,
using a stack view copied from the persistent view table. While enabled the mod
skips this renderer entirely, returning its normal success value of 0. This hides
both dirt and rain, including effects already present. No view fields or
persistent dirt/rain/tear-off state are written. Disabling resumes normal draws.

Installation checks beta21 PE identity, entry bytes, and the type/active gate.
`mxb_clean_lens_smoke` verifies the default state, hook readiness guard,
draw suppression, toggling and shutdown.
Live visual testing remains outstanding.

## Target
- `F:\steam\steamapps\common\MX Bikes\mxbikes.exe`, **beta21**, x64, OpenGL renderer
- IDB: `F:\steam\steamapps\common\MX Bikes\mxbikes.exe.i64` (ida-pro-mcp on 127.0.0.1:13337)
- ReShade is installed as `opengl32.dll` in the game folder — keep in mind for any GL hooks.
- Existing plugins in `plugins\`: `MXB_AKit_Studio.dlo`, `mx_replay_studio.dlo`, `proxy64.dlo`, `proxy_udp64.dlo`, `xinput64.dli`

## Data files (plain text, see `reference/`)
- Bike: `bikes/<id>/<id>.cfg` (engine, gearbox, driveline, wheel0/1 brakes, chassis, aero, steer,
  front_suspension, rear_suspension), `<id>.engn` (torque/brake curves vs rad/s)
- Tyres: `tyres/<type>/<type>.cfg` -> `*.tyre` (Pacejka-style `p_Dx1`/`p_Dy1` = peak long/lat friction,
  per-surface `dry_grip`/`wet_grip`, carcass `springBase`/`damper`)

## IDA string anchors (confirmed present in exe)
The parser looks up keys by path, so each string xrefs to the code that stores the value:
- Engine: `engine/IdleRPM`, `engine/BreakingRPM`, `engine/Limiter%d`, `engine/NumLimiter`, `engine/Clutch/MaxTorque`
- Gearbox/driveline: `Gearbox/UpShiftTime`, `Gearbox/EngageTime`, `Gearbox/DisengageTime`,
  `Gearbox/DisengageMaxTorque`, `Driveline/Efficiency`
- Chassis: `Chassis/FrontLatStiffness`, `Chassis/RearLatStiffness`, `Chassis/FrontCrashBar/*`, `Chassis/RearCrashBar/*`
- Steer: `Steer/Mass`, `Steer/InertiaBox`, `Steer/KYaw`
- Suspension: `Front_Suspension/*`, `Rear_Suspension/*` (`Damper/ThreeStage`, `Damper/BumpThreshold0`,
  `Preload/Setting|Range`, `RideHeight/Setting|Range`, `BumpStop/SpringBase`)
- Tyres: `p_Dx1`, `p_Dy1`, `springBase`, `%s/%s/dry_grip`, `%s/%s/wet_grip`
- Falling / world: `gravity`, `gravity_factor`, `rider_crash_mode`, `cc_crashspeed`, `overjump_crash`, `ID_CRASHMODE`, `fall_ref`
- Multiplayer (for the offline-only guard): `ID_MULTIPLAYER`, `SERVERCONNECTION`, `CLIENT_DATA`, `HOSTED`, `CC_SyncClientConnection`

## Design: works on every bike
- Stock (`bikes.pkz`) and mod bikes all go through the same `.cfg` parser into the same runtime
  bike object, so the module never touches files. It hooks the point after a bike (and its tyres)
  finishes loading and edits that runtime object.
- Settings are **multipliers** by default (power x1.2, grip x1.1, spring x0.9 ...) so they scale
  sensibly across a 125 2T and a 450 4T. Optional absolute overrides per field.
- Original values are saved per bike instance and restored on toggle-off or when the bike is unloaded.
- Writes are blocked whenever the multiplayer/session flag says we're connected to a server.

---

# Reverse-engineering map (beta21, imagebase 0x140000000)

All offsets below are confirmed from the IDB (renamed + commented there, IDB saved).

## Config parser primitives
| addr | name | signature |
|---|---|---|
| `0x1401569F0` | `Cfg_Open` | `(uint* outHandle)` -> 0 on success |
| `0x1401573A0` | `Cfg_Close` | `(uint handle)` |
| `0x140156EB0` | `Cfg_GetString` | `(h, char* dst, const char* fmt, ...)` |
| `0x140157060` | `Cfg_GetInt` | `(h, int* dst, const char* fmt, ...)` |
| `0x1401572B0` | `Cfg_GetFloat` | `(h, float* dst, const char* fmt, ...)` |
| `0x1401B7840` | `Cfg_GetDouble` | `(h, double* dst, const char* fmt, ...)` |
| `0x140157330` | `Cfg_GetFloatArray` | `(h, float* dst, int n, const char* fmt, ...)` |

Every getter returns non-zero when the key is missing, so a failed read leaves `dst` untouched.

## Handle registry  (`HandleRegistry_Alloc` @ `0x14015C040`)
```
+0            int32   count
+4  + 16*i    int32   id       (1-based handle)
+8  + 16*i    int32   padding
+12 + 16*i    void*   ptr
```
**Do not model this as a C++ struct.** Entries begin at +4, but any struct containing a pointer
is 8-byte aligned, so `struct { int32_t count; RegEntry entries[]; }` places them at +8 and every
pointer read lands 4 bytes off. That silently produced an empty bike list. Use explicit byte
offsets.
`g_BikeDataRegistry` = `0x140396AD8` holds bike **.cfg** data for the setup/garage code
(`sub_140120CC0`). It reads back **NULL in a running session**, so it is *not* how to reach a
live bike.

## Live entities: the per-world collections
`Physics_StepWorld` @ **0x1401BE3A0** is the authority:
```
world = g_Worlds (RVA 0x5677A0) + 24928 * (worldId - 1)      worldId in 1..3
```
Each world holds six collection pointers, identified by the update function applied to them:
| offset | update fn | contents |
|---|---|---|
| +8  | `sub_1401CF180` | |
| +16 | `sub_1401DEFE0` | |
| +24 | `sub_14017BA20` | |
| +32 | `sub_1401867D0` | |
| **+40** | **`Bike_Update`** | **bikes** |
| +48 | `sub_140166A10` | |

`Bike_Update` passes the element straight to `Rider_Update`, so the collection element **is** the
bike physics object. The supported readable prefix is 145896 bytes (through the last
validation flag), not a claim about the complete allocation size. Collections use the same 16-byte entry layout as the handle
registry (`Collection_Count` @ 0x14015C240, `Collection_At` @ 0x14015C350).

## `SettingRange` (`SettingRange_Init` @ `0x1401BF340`)
```c
struct SettingRange { float base; float step; int32_t numSteps; int32_t setting; }; // 16 bytes
```
Effective value = `base + step * setting`. Used for every `*/Range` + `*/Setting` pair
(springs, preload, ride height, bump stop, tyre pressure, ...).

## Live bike physics object (loader `0x14018FF80`, checksum `0x1401B5B30`)
The earlier map used a different loader at `0x140168CD0`. Its engine, driveline,
suspension, chassis and aero locations do not match the live bikes enumerated
from the world's collection. The map below was corrected against the live
loader, checksum and runtime samples on October 25. The checksum reads through
byte 145591; its optional validation flags extend through byte 145895.

### Engine  (-> **engine power modifier**)
| off | field |
|---|---|
| 1816 | `engine/id` (string) |
| 1848 | `engine/inertia` (double) |
| 2020 | number of engine mappings (max 5) |
| 2024 + 264*i | mapping[i]: `name`@+0, `short_name`@+100, **torque curve**@+208, **brake curve**@+232 |
| 3344 / 3348 | active mapping index |
| 3504 | `engine/Clutch/MaxTorque` (double) |
| 3648 / 3652 | `engine/MaxRPM` / `engine/ShiftRPM` (revolutions/second) |
| 1984 / 1992+8*i | `engine/NumLimiter` / `engine/Limiter%d` (double, revolutions/second) |
| 145576 / 145584 | `Aero/MinCdA` / `Aero/MaxCdA` (double) |

Curve record (`Curve_LoadFromCfg` @ `0x1401B78C0`, 24 bytes):
```c
struct Curve { int32_t type /*=12*/; int32_t _pad; int32_t numPoints; int32_t _pad2;
               struct { float x /*revolutions/second*/, y /*Nm*/; } *points; };
```
Power modifier = scale `points[k].y` of every mapping's torque curve. Engine braking = brake curve.

### Gearbox / driveline  (-> **transmission modifier**)
| off | field |
|---|---|
| 3664 / 3668+8*i | ratio table count / driving and driven teeth pairs |
| 12512 | `Gearbox/NumGears` |
| 12556 + 4*g | `Gearbox/Gear%d` (index into the ratio table) |
| 12864 / 12868 | `UpShiftTime` / `DownShiftTime` |
| 12872 / 12876 | `DisengageTime` / `DisengageMaxTorque` |
| 12888 / 12892 | `EngageTime` / `EngageMaxSpeedDiff` |
| 12904 | `SemiAutomatic` |
| 13508 / 13932 | front / rear sprocket setting |
| 13528 / 13532+4*i | rear sprocket table count / teeth |
| 13960 / 13984 | `Driveline/FirstAxisInertia` / `SecondAxisInertia` (double) |
| 14016 | `Driveline/Efficiency` (double) |

### Suspension  (-> **suspension modifier**)
| off | field |
|---|---|
| 16672 | `Front_Suspension/Mass` (double) |
| 16720 | `Front_Suspension/Spring` — `SettingRange` |
| 19464 | `Front_Suspension/Oil` — `SettingRange` |
| 19496 / 19516 | front `Preload` / `RideHeight` — `SettingRange` |
| 18384 / 18392 | front damper bump thresholds (double) |
| 19400 / 19408 | front damper rebound thresholds (double) |
| 19540 / 19544 | front `TopOutSpring/Length` / `K` |
| 20408 | `Rear_Suspension/Mass` (double) |
| 20896 | `Rear_Suspension/Spring` — `SettingRange` |
| 23652 / 23672 / 23692 | rear `Preload` / `RideHeight` / `RodLength` — `SettingRange` |
| 22552 / 22560 | rear damper bump thresholds (double) |
| 23568 / 23576 | rear damper rebound thresholds (double) |
| 23632..23644 | rear `BumpStop/SpringBase`, `SpringRisingRate`, `DampingBase`, `DampingRisingRate` |
| 23712 / 23716 | rear `TopOutSpring/Length` / `K` |

### Chassis
864/872 `FrontLatStiffness`/`Damping`, 880/888 `FrontLong*`, 896/904 `RearLat*`,
912/920 `RearTorsional*` (all doubles). 832 `Chassis/Mass`, 852 `Chassis/Inertia[3]`.
`Chassis/*CrashBar/*` is **car/kart only** — it does not exist on the bike path.

### Tyres  (embedded in the bike object; `Bike_LoadTyres` @ `0x14018FB00`)
| off | field |
|---|---|
| 24832 + 58128*wheel + 4912*i | tyre record `i` for that wheel (10 max) |
| 73952 / 132080 | active tyre index, wheel 0 / 1 |
| **73960 / 132088** | **active tyre pointer** (qword), wheel 0 / 1 |
| 73968 / 132096 | `wheel%d/Friction` (double) |

### Rider  (`Rider_LoadCfg` @ `0x140192EC0`)
142532 `Rider/id`, 142588 `Rider/Type`, 142800 rider mass (double), 142820 rider type,
142828 **crashed flag**, 142832 `fall_ref[3]` (detach *position* reference, not a threshold),
142844 `detach_blend`, 144396 rider state (10/11 = just crashed), 145280.. rider spring/lean params.

## Tyre record  (`Tyre_ParseBike` @ `0x14018E8F0`, 4912 bytes)  (-> **tyre grip modifier**)
| off | field |
|---|---|
| 136 / 160 / 176 | `Mass` / `Radius` / `Width` (doubles) |
| **208** | **`p_Dx1`** — peak longitudinal friction |
| **276** | **`p_Dy1`** — peak lateral friction |
| 212 / 280 / 284 | `p_Dx2` / `p_Dy2` / `p_Dy3` |
| 676 | number of compounds |
| 680 + 688*c | compound `c`, name at +0 |
| &nbsp;&nbsp;+100 + 32*s | `dry_grip` for surface `s` |
| &nbsp;&nbsp;+104 + 32*s | `wet_grip` |
| &nbsp;&nbsp;+108 / +112 + 32*s | `long_stiffness` / `lat_stiffness` |
| &nbsp;&nbsp;+116 / +120 + 32*s | `resistance_dry` / `resistance_wet` |
| &nbsp;&nbsp;+124 / +128 + 32*s | `compacting_factor` / `bulldozing_factor` (the latter is stored x10000) |
| 4840 / 4848 / 4856 | `SpringBase` / `SpringFactor` / `Damper` (doubles) |

Surface index order (0..14): asphalt1, asphalt2, asphalt3, concrete, grass, sand, kerb, soil,
paint, artificial turf, soft soil, compact soil, gravel, rock, (15th unused).
MX-relevant: 4 grass, 5 sand, 7 soil, 10 soft soil, 11 compact soil, 12 gravel.

## Crash tolerance  (-> **crash tolerance modifier**)
`Rider_Update` @ `0x1401A8480` calls `Rider_Detach` @ `0x1401A79F0` when:
```c
if ( fabs(sumLat)  > 4000.0                       // double @ 0x140329410
  || fabs(sumVert) > 10000.0                      // double @ 0x140354118
  || (!*(int*)(*(void**)(bike+8) + 24892)         // gate: non-zero disables the next two
      && (sumLong > 15000.0                       // float  @ 0x140328210
          || avg25 < -2000.0)) )                  // float  @ 0x14032940C
    Rider_Detach(bike);
```
**There are THREE detach exits in `Rider_Update`, not one:**
| site | condition | how to disarm |
|---|---|---|
| `0x1401AD30E` | the four force thresholds | scale the constants (see above) |
| `0x1401A85C4` | `Rider_GroundQuery` + height delta <= 0, i.e. rider at/below the surface | **do not suppress** -- see below |
| `0x1401A8799` | `fabs(lean) > 75.0` (`qword_140354060`) and `[bike+0x22DE4] == 0` | patch the skip jump at `0x1401A8794`, `jbe` `0x76` -> `0xEB` |

The 75.0 is shared with `sub_1400D8550`, `sub_14016C640` and `Rider_LoadCfg` (which divides by
it), so do not edit it -- patch the branch.

**The ground-contact exit must not be suppressed.** `Rider_GroundQuery` fills a surface-contact
record at `bike+144652`; the test means the rider is inside the terrain, and detaching is the only
response the solver models for that. Forcing the skip leaves the rider embedded and the next stage
ejects it -- observed as the bike launched with the camera below the surface after a soft landing,
then a NaN position. True "never crash" needs auto-recovery after the detach, not suppression.

## Rider reset  (`Rider_Reset` @ `0x1401957D0`)
`int __fastcall Rider_Reset(void* bike)` -- one pointer in rcx, no float args. Puts the rider
back on without moving the bike:
```
bike+142828 = 0    crashed flag
bike+144396 = 0    rider state -> attached
bike+144404 = 0
re-seats the rider transform from bike+928 into bike+142996
clears the rider velocity/history buffers and leg/arm attachment flags
```
Dereferences the pointer at `bike+144512`, so validate that first.
`Bike_PlaceOnTrack` @ `0x140197060` calls it at `0x140197A45` with rcx = bike and nothing else,
which is why it is usable standalone.

**The detach branch also aborts the caller.** The site reads
`if (threshold) { Rider_Detach(a1); return 1; }`, so it returns out of `Rider_Update` before the
remaining rider physics runs. Suppressing `Rider_Detach` alone therefore still stops the rider
being simulated on every frame the thresholds are exceeded -- observed as the bike being launched
on landing, and a NaN position faulting the terrain sampler at `0x1401F1923`. To disable crashing,
raise the thresholds so the condition never fires; do not neuter the function.

Patchability:
- `0x140329410` (4000.0) and `0x14032940C` (-2000.0) are referenced **only** by this check ->
  safe to overwrite the constant in place.
- `0x140354118` (10000.0) and `0x140328210` (15000.0) are **shared** with other functions ->
  do *not* touch the constant. Patch the 4-byte RIP-relative displacement of the instruction
  instead: `comisd` @ `0x1401A9CB2` and `comiss` @ `0x1401A9F6D`, pointing them at our own floats.
- `*(int*)(*(void**)(bike+8) + 24892)` acts as an existing "ignore long/sustained crash" gate.

## Session / offline guard  (resolved)
`g_NetConnectionState` @ **RVA 0x3D7900** is the raw network client state. Written only by
`Net_ClientUpdate` (`0x1402A6890`) and `Net_BeginConnect` (`0x1402A7860`):
```
0 = client never brought up     1 = disconnected / idle
2 = connecting                  3 = connect established   4 = connected
```
`Net_QueryConnectionState` (`0x1402A79A0`) collapses that to what the UI sees
(raw 0/1 -> 0, raw 2 -> 1, raw 3/4 -> 2). The module reads the raw global; treat 0/1 as offline
and everything else as blocked.

**Do not call `Net_QueryConnectionState`.** Its signature is
`(int* outState, int* outReason, char* outMessage)` — three out-params. Calling it with two
leaves `r8` holding garbage and it `strcpy`s through it, faulting at `0x1402A79F6`. This crashed
the game on the first inject. General rule for this module: read and write game memory, never
call game code.

## Not us: A_Kit_Studio setup exports
`MXB_AKit_Studio.dlo` in `plugins\` is a setup advisor that **writes setup files**. Its journal
(`pluginskit_studio_data\`, and `setup_advisor_journal.txt` beside each bike's setups) says
"Writes on export" / "queued for next F9 export", and it creates setups named
`A_Kit_Studio-(<track>)#N.stp/.stt` under
`Documents\PiBoSo\MX Bikes\profiles\<profile>\setups\<track>\<bike>\`, then points that
bike's `default.ini` at one.

A bike behaving badly *with nothing injected* is most likely running one of those exported setups,
not anything this module did -- it has no file I/O at all beyond its own log. Check the setup
timestamps and `default.ini` before suspecting the module.

## Still open
- Where the loaded suspension `SettingRange` values are baked into the solver (if they are cached
  per-step, writes need to land before/at that point rather than once after load).
- The consumer of the cvar-name table at `0x1403953F0` (`cc_crashspeed`, `CC_Rainy`, ...).
- Hooking `IDirectInputDevice8::GetDeviceState`, so the menu can take input away from the
  bike itself. The collector hooks below only cover the game's UI input.

## Host facts worth keeping
- The game imports **`SwapBuffers` from GDI32**; `wglSwapBuffers` appears nowhere in the binary,
  so that is the hook target.
- The GL import set is pure fixed function (`glBegin`, `glVertex3f`, `glMatrixMode`, `glOrtho`,
  `glMaterialfv`, `glLightfv`, `glTexGeni`, `glEnableClientState`) with no shader/VAO/VBO entry
  points, i.e. a compatibility profile context. A GL 1.1 renderer backend is the right fit;
  `imgui_impl_opengl3` is not usable here.
- Input comes in through **DirectInput8** (`DirectInput8Create` is imported), not just window
  messages.
- `App_WinMainLoop` @ **0x140136710** only advances a frame when the message queue is empty:
  `while (!PeekMessage) { App_Frame(dt); }`, with the drain loop jumping back in only once
  `PeekMessage` returns false. `App_WndProc` @ **0x1401321C0** always ends in `DefWindowProcA`.
  Together these mean an overlay must never swallow messages in a window subclass here --
  doing so froze the game until the menu was closed.
- The game's UI input collectors, both called from `App_WndProc` for every message:
  `Input_CollectKeyboardMsg` @ **0x28DFD0** (key/char ring buffer at `g_UiKeyQueue` 0x140586C80,
  count at 0x140586C70, plus Ctrl+V clipboard paste) and `Input_CollectMouseMsg` @ **0x28E1F0**
  (button bits 0x140586EA0, normalised cursor pos 0x140586E90, wheel accumulator 0x140586E98).
  No-oping these is how to take input away from the game without touching the pump.
- `App_Frame` @ **0x140135CD0** skips its whole update+render block when
  `sub_1400275A0(dt, 0, 1)` returns non-zero -- worth knowing if frames run but nothing draws.
- The game is built against **MSVCR90** (VC9 CRT), which is why our DLL links the CRT statically.
- The game renders with its own client arrays and **multitexturing**. Any overlay drawing with
  `glDrawElements` must disable every client array it does not use and reset the active client
  texture unit to 0 first, or the driver reads the game's stale pointers with the overlay's
  indices (observed: `nvoglv64.dll` access violation on track load) and texcoords land on the
  wrong unit (observed: textured geometry invisible while untextured drew fine).
- The game's main window handle is stored at **RVA 0xE4B238** (`hWndParent`, set by
  `App_CreateWindow`). Prefer it over `WindowFromDC`: a throwaway window is created while
  probing pixel formats.
- The game can recreate its GL context (`wglCreateContext` / `wglDeleteContext` /
  `wglShareLists` are all imported), which invalidates any texture names an overlay holds.

## FOV, properly: View_BuildFrustum (0x140249510)

The glFrustum hook was the wrong level. `View_BuildFrustum(view)` is the single
place a field of view becomes geometry, and it is called once per view per frame
(view objects are stack-allocated in `Render_Frame`, so the dirty flag at
`view+16` is always 0 on entry).

Its normal-perspective branch reads the FOV **in degrees from `camera+4`**, where
`camera = *(void**)(view+48)`:

    aspect   = (view+20 / view+24) * (*g_Screen + 32)
    halfTanX = tan(fov * pi/180 * 0.5)
    if (aspect > 1.3333) halfTanX *= aspect / 1.3333     // camera+4 is 4:3-referenced
    halfTanY = halfTanX / aspect
    near     = tan(10deg) / halfTanX * (view+104)

and writes, from those four numbers:

| offset | what |
|---|---|
| +108 | near |
| +72/76/80/84 | near-plane L/T/R/B — the only thing glFrustum consumes |
| +88/92/96/100 | far-plane L/T/R/B |
| +116 | world → window projection matrix (64 bytes) |
| +208..+300 | the 8 frustum corner points |
| +304..+399 | the 6 **culling planes**, built from those corners |

**That last row is the peripheral bug.** Rescaling the planes inside a glFrustum
hook widened the projection and nothing else, so geometry only visible in the
newly exposed edges was still being culled against the stock frustum. On top of
that, three glFrustum calls take the player's view object — one in
`Render_MainView` (0x14024F17A) and two in `Render_ViewPasses` (0x14024FAD9,
0x14024FF02) — and a return-address filter scoped to `Render_MainView` caught
only the first, so two passes kept projecting at the stock FOV.

Overriding `camera+4` around the original call fixes projection, far extents,
corners and culling together. Non-player views exclude themselves with no
call-site filtering needed, because their branches never read `camera+4`:

- `view+192 == 1` — the reflection recursion (`Render_ViewPasses`' `Src` copy),
  derives its planes from the existing ones
- `view+192 == 2` — explicit bounds at `view+56..+68`
- `view+196 != 0` — ortho
- `view+188 == 1 && view+400 == 1` — `Render_CubeProbe`'s six faces, which set
  `camera+4 = 90.0` themselves

Save `camera+4`, write, call, restore: the camera object is shared, so leaving
the value in it would persist past the frame.

## Matrix convention

`Mat_TransformPoint(out, m, v)` (0x140266D80) settles it for every matrix in the
engine:

    out.x = v . (m0,m1,m2)  + m3
    out.y = v . (m4,m5,m6)  + m7
    out.z = v . (m8,m9,m10) + m11

Row-major, **translation at float index 3/7/11** — not at the GL column-major
12/13/14. `glLoadMatrixf` is handed these arrays raw, so OpenGL reads the
transpose; don't reason about the engine's matrices through the GL path.

**Direction is the trap, not the layout.** The layout above holds everywhere, but
whether a given matrix is local→world or its inverse has to be established per
matrix, and the engine stores plenty of inverses.

`View_BuildFrustum` settles the camera, and the argument order is the tell. It
opens with `invert(camera+32, camera+96)` — dst first, like every other helper
here — so **`camera+32` is derived from `camera+96`**, not the other way round.
It then pushes the eight frustum corners through `camera+96` to build the
world-space culling planes. So:

- `camera+96` — camera→world (the placement transform)
- `camera+32` — its inverse, i.e. the view matrix. This is why
  `view+116 = S * camera+32` takes **world** points.
- `sub_140266750` = invert, `sub_1402666D0` = transpose (what converts the
  engine's row-major into the column-major order `glLoadMatrixf` wants).

- `bike+928` — the bike's transform, and **its translation is NOT the bike's
  position**. It is the inverse, like `camera+32`: the translation is `-R*pos`,
  so reading `+940/+944/+948` as a position yields the right magnitude with the
  axes scrambled. It renders as a fixed point far outside the map, which is
  exactly how this was found. `Rider_Reset` re-seating `bike+928` into
  `bike+142996` is not evidence either way.

### Bike world position: use the wheel midpoint

`bike+75720` is a plain world-space `float[3]` (x, y, z), `+ kWheelStride`
(58128) for wheel 1. No convention guess needed, because
`Bike_BuildStateSnapshot` takes the midpoint of the pair and writes it straight
into the translation slots of a local→world frame:

    Mat_Copy64(tmp, bike + 145984);
    tmp[3]  = (bike[75720] + bike[133848]) * 0.5;   // x
    tmp[7]  = (bike[75724] + bike[133852]) * 0.5;   // y
    tmp[11] = (bike[75728] + bike[133856]) * 0.5;   // z
    invert(tmp, tmp);
    Mat_TransformPoint(out, tmp, worldCentreOfMass);

Writing a world position into slots 3/7/11 is what confirms both the layout and
that these are world space; inverting the frame to express the world centre of
mass inside it is what confirms the direction. The midpoint is also the engine's
own reference point for a bike, so it is the right thing to hang an overlay off
— bearing in mind it sits at wheel level, not at the bike's origin.

There is a second pair at `bike+75784` / `+133912` (offset by 64), probably hub
centre against contact patch. Either works as an anchor.

World up is +Y, confirmed by the `{0,1,0}` up vector in `Render_MainView`.

### Projecting a world point without touching GL

`view+116` is the engine's own world → window matrix, built as `S * camera+32`
where `S` is identity plus

    m0 = vpW / (2*halfTanX)    m2 = vpW*0.5 + vpX
    m5 = vpH / (2*halfTanY)    m6 = vpH*0.5 + vpY

Push a world point through it with the convention above and divide x and y by z:

    px = p.(m0,m1,m2) + m3      sx = px / pz
    py = p.(m4,m5,m6) + m7      sy = py / pz
    pz = p.(m8,m9,m10) + m11    (view-space depth; reject pz <= near)

Both come out as **fractions of the render target**, x left→right spanning
`[vpX, vpX+vpW]` and y **bottom→top** spanning `[vpY, vpY+vpH]` (GL window
convention), so a top-left pixel is `(sx*W, (1-sy)*H)`. Derived by evaluating the
matrix at the frustum edges, which means it holds regardless of whether
`camera+32` is local→world or its inverse — no handedness guess required.

## World look: intercept the setters, don't poke the globals

The three engine setters each cache into `*g_GlStateCache` and then call GL, and
each is fed per frame from a global that weather and time-of-day rewrite. Hooking
the setter rewrites the value *as it is applied*, so nothing needs saving or
restoring and it does not matter how often the game recomputes it.

| hook | signature | drives |
|---|---|---|
| `GL_SetClearColor` 0x14022FB50 | `(float r, float g, float b)` | sky / background, from `g_ClearColor` 0x14057CC50 |
| `GL_SetFogColor` 0x140228CF0 | `(float rgb[3])` | `glFogfv(GL_FOG_COLOR)`, from `g_FogColor` 0x14057CC10 |
| `GL_SetFogEnable` 0x140228C90 | `(int on)` | `glEnable/glDisable(GL_FOG)` |
| `opengl32!glFogf` | pname `0xB62` | fog density, from `g_FogDensity` 0x14057CB70 |
| `opengl32!glLightfv` | `GL_LIGHT0`, `GL_AMBIENT` | world light level, from `g_SunLight` 0x14057CC40 |

Watch the `glLightfv` pname: `Render_MainView` calls
`glLightfv(0x4000, 0x1200, ...)` and **0x1200 is `GL_AMBIENT`**, not `GL_DIFFUSE`
(0x1201). Together with `glLightModelfv(GL_LIGHT_MODEL_AMBIENT, {0,0,0,1})` being
hard-coded to zero, that means the world's light level is carried entirely by
`GL_LIGHT0`'s **ambient** term. A hook that filters on `GL_DIFFUSE` never fires,
which is exactly the bug the first version of this shipped with.

Copy any array argument to a local before modulating it; the caller's buffer is
its own stack frame and the engine reads it again after the setter returns.

Note ReShade is installed as `opengl32.dll` here, so `GetProcAddress` on it
returns ReShade's wrapper. Hooking there is still correct (we sit above it), and
a missing export just makes the hook best-effort.

## Outgoing GUID player-info projection (October 25, 2026)

The profile GUI string at `0x140E5522C` is a hex rendering of a **nine-byte**
value, not a Windows GUID. Init `0x1400EF210` obtains the local representation
at `0x140E55220`, formats bytes in reverse order for the profile, and submits a
decoded copy to engine API 783. The gameplay payload builders `0x14005FF60`
and `0x140095D70` copy/decode it at byte **648** of a **684-byte** player-info
payload. The following nine bytes at 657 and the later session fields are
separate data and must not be overwritten.

Engine dispatcher `0x140120CC0` routes API 840 to `0x1402A21A0`
`(connection, payload, length, callback)`, API 850 to `0x1402A47A0`
`(payload, length)`, and API 869 to `0x1402A4640` `(payload, length)`.
The first two synchronously copy payload bytes to `0x140598AA8`; the third
synchronously serializes a type-15 player-info message and queues it. The
hooks pass a temporary replacement copy through those consumers, preserving
the original buffers and globals. Disabled cases pass through; enabled cases
with unreadable/wrong-size/identical-GUID payloads block that call. Prefix and
PE checks gate all three hooks together.

`Net_BeginConnect` at `0x1402A7860` is a separate LOGIN path, not the gameplay
payload consumer, and is not hooked. Server join handling `0x1400787C0`
contains an explicit `GUID spoofing` rejection path. Local projection tests
establish byte preservation and correct endianness only; they do not establish
that a remote server accepts a replacement or conceals other identity fields.
