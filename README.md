# MXB Nemora

An injectable C++17 mod for **MX Bikes beta21f on Windows x64**, with an ImGui
overlay, physics modifiers, visual controls, clean lens, Spinny, network ram,
and diagnostics. Physics settings multiply the loaded bike's own values so
the same controls work across different bikes. Feature behavior and current
limitations are documented below.

This repository distributes **source code only**: the mod, standalone tests,
build scripts, documentation, and the menu/ImGui source required to compile it.
Compiled DLLs, executables, debug symbols, logs, screenshots, IDA databases,
and reference game data are excluded. Build the DLL locally using the steps below.

Requirements: Windows x64, Visual Studio 2022 with the C++ desktop workload and
Windows SDK, CMake 3.20 or newer, and vcpkg with `minhook:x64-windows-static`.
The binary offsets and hook checks target beta21f; other game builds require
revalidation. The build checks the executable identity before installing hooks.
Detailed implementation notes below may refer to the preceding beta21e build.
The current source enables `MXB_TESTING_NO_GUARDS` in
`src/mxb/Modifiers.h`; see **Testing build switch** before building.

| Modifier | What it touches |
|---|---|
| Tyre grip | `p_Dx1` / `p_Dy1` and every per-surface `dry_grip` / `wet_grip`, plus carcass spring and damper |
| Suspension | front/rear spring rate, bump and rebound thresholds, bump stop, top-out spring |
| Engine power | every point of every mapping's torque curve, the brake curve, clutch max torque |
| Transmission | gear ratios, final drive, shift and clutch times, driveline efficiency and inertia |
| Crash tolerance | the four impact thresholds in `Rider_Update` that make the rider let go |
| Spinny | outgoing bike/rider orientation and a wheel clearance lift, controlled from VIS |

`NOTES.md` has the full reverse-engineering map: offsets, how each one was
derived, and the IDB function names they correspond to.

## Layout

```
src/mxb/Offsets.h      every RVA and struct offset, one place to re-derive on a game update
src/mxb/Process.*      module base, the engine's handle registry, loaded-bike enumeration
src/mxb/BikeData.h     typed field access over a live bike physics object and its tyre records
src/mxb/Session.*      offline-only guard
src/mxb/Modifiers.*    snapshot / apply / restore for the four data-driven modifiers
src/mxb/CrashPatch.*   the one modifier that patches code rather than data
src/ui/ModMenu.*       the overlay: one tab per modifier plus a status tab
src/ui/ModMenuControls.h  the reference menu's control macros, without its Config.h dependency
src/host/Hooks.*       MinHook on gdi32!SwapBuffers, window subclass, clean removal
src/host/Overlay.*     ImGui context, fonts, background texture, per-frame work
src/host/imgui_impl_gl2.*  fixed-function GL renderer backend
src/compat/Globals.h   stub for a leftover include in the vendored ImGui fork
src/dllmain.cpp        injectable entry point, INSERT to toggle, END to unload
src/Host.h             the per-frame contract, kept as documentation
```

## Building

The GitHub repository currently excludes `skrt menu clone/`, the custom ImGui
fork and menu headers required by CMake. Restore that dependency locally before
building a fresh checkout. The existing local workspace retains it.

```powershell
vcpkg install minhook:x64-windows-static
.\build.ps1
```

Output is `build\Release\mxb_physics.dll`. It links the CRT and MinHook
statically, so the only dependencies are system DLLs (opengl32, user32, ole32,
kernel32, imm32) and nothing has to sit next to the game.

## Finding moved functions after an update

With the game running, use `Get-Process mxbikes` to find its PID, then run
`python tools/scan_offsets.py <PID>`. The read-only scanner searches the
unpacked runtime code using the patterns in `tools/signatures.json` and prints
candidate RVAs. Missing or multiple matches are reported as unresolved. It
does not change the game or automatically update the cheat: function semantics,
call sites, globals, and structure layouts still need verification. Patterns
were checked against beta21e and beta21f and can break on later updates.

## Using it

Inject `mxb_physics.dll` into `mxbikes.exe` with any injector. Then:

- **INSERT** toggles the menu
- **F7** saves the current server rejection codes, integrity summary, and active mod settings. Press it immediately after a ban while the game is still open. The INFO tab also has a **Save ban diagnostics (F7)** button and save confirmation.
- **END** unloads: the overlay tears down on the render thread, bikes are
  restored to stock, the hook comes off, then the DLL frees itself

Diagnostic captures append to `mxb_physics.ban-diagnostics.log` beside the injected DLL and are also copied to the regular log. The diagnostic file survives re-injection. Captures read game state only; old reason codes can persist, and the client does not receive every detail of a server-side check.

## Spinny

Open **INSERT → VIS → Spinny → Enable Spinny**. It starts disabled. The whole
networked bike and rider point straight up and spin around the vertical axis at
**1.5 turns per second** (540 degrees/second). **Disable Spinny** returns the
next outgoing state to the game's normal pose. Local physics and camera state
are never written by this feature.

The outgoing bike and rider move up together by the amount needed to keep both
tyre bounds at least **20 cm above the higher original tyre bottom**. The
calculation uses current wheel centres from the snapshot and the local bike's
active tyre radii. This reference follows the bike over slopes and jumps; it is
not a terrain query at the remotely rotated wheel positions. Missing geometry
causes an unchanged outgoing state, with a skip reason recorded in F7 diagnostics.

Spinny is included in `mxb_physics.dll`; no separate controller or Spinny DLL is
needed. Start without the old standalone `spinny_hook.dll`: the integrated hook
detects it and stays unavailable to avoid two Spinny transforms. The menu then
shows the restart instruction. END disables and removes the integrated hook.

This hooks the local multiplayer client's verified send call. Hosting uses a
different path. Build and offline pose/controller tests pass; receiver-side
appearance and live multiplayer acceptance still need a two-client test.

```powershell
cmake --build build --config Release --target mxb_spinny_smoke mxb_local_bike_smoke
.\build\Release\mxb_spinny_smoke.exe
.\build\Release\mxb_local_bike_smoke.exe
```

## Network ram

Open **INSERT → VIS → Network ram → Enable network ram** (scroll down in VIS).
It starts disabled. The outgoing bike cycles through all active remote players
in slot order, spending **0.08 seconds** sweeping six metres through each
player's position at a relative **75 m/s**. Each sweep starts three metres in
front of the target and travels toward their rear. The transmitted bike starts
**90 degrees to the target bike's heading** and spins around its upright axis
at **6 turns per second** in the transmitted pose. Spin time continues across
target switches. The game caps transmitted positive angular velocity at 10 rad/s,
so receiver interpolation can limit the apparent spin between pose updates.

**Prediction lead** starts at **220 ms**, adjustable from **0–500 ms**. It adds
the target's current horizontal velocity times that interval, capped at twelve metres. This
is a manual prediction setting, not a measured ping or guaranteed lag correction.
Each outgoing update copies the target's current snapshot Y exactly; vertical
prediction is disabled so climbing/falling targets don't introduce a height offset.

**Bike only (limits spin)** starts **off**: the original rider state is preserved
so an attached rider follows the bike remotely and the receiver can use angular
velocity. Turning this option on marks the outgoing rider as detached and keeps
their original absolute pose, but the receiving game's physics proxy explicitly
zeros angular velocity for detached riders. A genuine local crash can also limit
remote spin until recovery. Local physics, camera and crash state stay unchanged.

**This does not hide your nametag:** the game positions it above the bike's wheel
midpoint, even when the rider is elsewhere. Other clients may still associate
the bike with your name. The menu's **Outgoing yaw** shows the angle being sent;
F7 includes that angle, its rate and the `fast_spin_v3_ylock` build marker for diagnosis.

This controls one outgoing bike and cycles targets. It does not create multiple
simultaneous network bikes. The inspected reset/reposition path reuses the
existing bike, while network entity allocation and ownership are managed by the
server. Creating local copies alone does not add replicated server entities.

This shares Spinny's verified local gameplay-client serializer hook. While
network ram is enabled it takes priority over Spinny; disabling it resumes
Spinny if that setting is still enabled. Hosting is unsupported. Missing targets,
unreceived states, invalid geometry, and disconnects leave the outgoing state
unchanged. Departed players are skipped and reused slots are checked by rider ID.
END disables the feature and removes the hook.

Remote player identity comes from the roster ID; an empty local physics handle
in a remote slot is valid. The menu shows the target slot/count and how many
remote slots are receiving data and have snapshots. If detection fails, the
status distinguishes missing slots, missing received state and rejected poses.
F7 records these detection counts, the target, prediction setting, changed outgoing states, skips
and status. These counters measure local state
rewrites, **not confirmed hits**. The receive path feeds remote poses and velocity
into physics proxies, but collision results and multiplayer acceptance still
need a two-client test. Faster cycling does not guarantee a hit: packet timing,
interpolation and the receiving client's collision rules still apply.

```powershell
cmake --build build --config Release --target mxb_network_ram_smoke
.\build\Release\mxb_network_ram_smoke.exe
```

## Clean lens

Open **INSERT → VIS → Lens → Clean lens**. It starts disabled and hides both
visor dirt and rain on the next frame, including effects already on screen.
Turning it off shows the game's current dirt and rain again; it does not consume
tear-offs or clear the underlying effect state. END removes the hook.

The hook requires the known beta21 executable identity and matching visor-render
instructions; **Visor hook unavailable** means it could not be installed.
The standalone test checks draw suppression, toggling, the hook guard and
shutdown. **In-game visual behavior still needs testing.**

```powershell
cmake --build build --config Release --target mxb_clean_lens_smoke
.\build\Release\mxb_clean_lens_smoke.exe
```

## Outgoing GUID

Open **INSERT → INFO → Outgoing GUID** before joining. A random replacement is
selected on injection; the spoofer starts disabled. Use **Random GUID** or enter
1–18 hexadecimal digits and click **Apply GUID**, then **Enable GUID spoofer**.
Reconnect after changing or disabling it. Selection is kept only until unload.

This replaces the nine GUID bytes in a separate copy of the beta21 684-byte
player-info payload during gameplay connection, queued player-info updates, and
immediate player-info sends. The stored client GUID and profile display stay
unchanged. The payload's other fields, including session data, are preserved.
It does not change the master-server login identity. The counter confirms that
a local outgoing payload was replaced, not that the server accepted the GUID.
**Live server acceptance has not been tested.**

Hooks require the known beta21 PE identity and matching entry-point bytes. An
unavailable hook disables the feature. While enabled, an unreadable source,
unexpected payload size, or already matching GUID blocks that player-info call.
While disabled, the game sends its usual data. Enable before joining; this
does not redact data already queued or sent, or conceal other identity fields.
END removes the hooks. Run the isolated payload/controller checks with:

```powershell
cmake --build build --config Release --target mxb_guid_smoke
.\build\Release\mxb_guid_smoke.exe
```

## Stock bike verification replies

The current build uses the loaded bike's captured stock parameters when MX Bikes builds the local client's verification summary. The INFO tab shows **Stock verification: ready**, the number of stock replies, and the number of checks for which a stock view was unavailable. Each stock reply is recorded in `mxb_physics.log` with the actual and stock bike checksum contributions.

The response uses a separate copy of the bike and engine curves, with the active tyre pointers directed into that copy. The live bike keeps its current modified values. Current setup selections and the game's live validation flags are preserved; only the bike checksum contribution inside this response callback uses the stock view. Other checksum callers, world/configuration summary components, setup validation, and the separate reported-value checks are unchanged.

The original values must have been captured before modification. A missing baseline, changed engine/curve identity, changed data layout, or unresolved active tyre causes the response to use the actual values. The specific failure reason is logged and included in F7 captures, with the stock reply count and last actual/stock checksum. Start from a clean game/bike load; this cannot recover originals that another mod already changed before capture. Re-inject the rebuilt DLL to load this implementation.

The `live_layout_2` build corrects the engine, gearbox, suspension and aerodynamic locations against loader `0x14018FF80` and checksum `0x1401B5B30`. Earlier offsets came from a different loader at `0x140168CD0`; they made the stock view reject the live curves. The copied prefix now covers every checksum read, and restoring stock values is exact rather than clamping unused fields.

Build validation covers stock projection, independent curve/tyre storage, unchanged live physics, setup selections, and stale/missing baseline guards. **Live multiplayer acceptance is not yet verified.** The server may reject for a different check; this is not a general anti-ban guarantee.

The projection smoke test uses a synthetic beta21f bike layout. The older
October 25 live-bike fixture belongs to the previous layout and is not replayed
by this build.

Run the standalone projection test with:

```powershell
cmake --build build --config Release --target mxb_stock_view_smoke
.\build\Release\mxb_stock_view_smoke.exe
```

## Host notes

**Hook target is `gdi32!SwapBuffers`, not `wglSwapBuffers`.** mxbikes.exe imports
`SwapBuffers` from GDI32, and the string `wglSwapBuffers` does not appear in the
binary at all, so a hook on it would never fire.

**The renderer backend is fixed-function GL, not opengl3.** The game's import
table is `glBegin` / `glEnd` / `glVertex3f` / `glMatrixMode` / `glOrtho` /
`glMaterialfv` / `glLightfv` / `glTexGeni` / `glEnableClientState` with no
shader, program, VAO or VBO entry point anywhere, which means a compatibility
profile context. That is precisely what `imgui_impl_opengl3` cannot target: it
needs GL 3.2+ core, a GLSL pipeline and a function loader. `src/host/imgui_impl_gl2.cpp`
uses only GL 1.1/1.2 calls that opengl32.dll exports directly, so there is
nothing to load and nothing to vendor, and it saves and restores the GL state
around every frame so the game's own rendering is untouched.

**ReShade is left alone.** It occupies `opengl32.dll` in the game folder; we
neither proxy nor re-export it. One consequence: our overlay draws before
ReShade's present path, so ReShade's effects apply to the menu too.

**The main loop gates frames on an empty message queue.** `App_WinMainLoop`
(`0x140136710`) is:

```c
while (!PeekMessageA(&msg, 0,0,0, PM_REMOVE)) {
LABEL_7:
    ...sleep to frame budget...
    App_Frame(dt);          // the only place a frame advances
    continue;
}
while (msg.message != WM_QUIT) {
    TranslateMessage; DispatchMessageA;
    if (!PeekMessageA(...)) goto LABEL_7;
}
```

A frame runs **only** when the queue drains. On top of that, `App_WndProc`
(`0x1401321C0`) always ends in `DefWindowProcA`. Any overlay here has to leave
message dispatch alone — see "The freeze" below.

**Input isolation hooks the game's collectors, not the window proc.**
`App_WndProc` feeds every message to `Input_CollectKeyboardMsg` (`0x28DFD0`, a
key/char ring buffer) and `Input_CollectMouseMsg` (`0x28E1F0`, button bits,
normalised cursor position, wheel accumulator). Making those two no-ops while
the menu is open takes input away from the game's own UI without touching the
pump. A mouse button held at the moment the menu opens leaves its bit set until
the next real button message.

**Two repairs to the vendored reference menu were needed to make it build:**

- `skrt menu clone/menu/MenuFonts.h` was corrupt. Lines 31-213 were a font's raw
  data with no declaration above them, which made the header uncompilable. The
  block is 2191 dwords and starts with the same `0x0000bc57` stb magic as the
  other two fonts, so it is a third compressed font, most likely the tab icons
  the READ ME says were left out. Only the missing declaration and a `sizeof`
  based size were added; no data was removed and no byte count was guessed.
- The ImGui fork has a leftover `#include "Globals.h"` in `imgui.cpp` and expects
  five font globals by extern (`menuFont`, `boldMenuFont`, `controlFont`,
  `comboFont`, `tabFont`). `src/compat/Globals.h` is an empty stub for the
  include; `Overlay.cpp` defines all five fonts.

## How it works

`Tick()` walks `g_BikeDataRegistry` (RVA `0x396AD8`), which is the engine's
generic handle registry — every loaded bike `.cfg` is one entry pointing at a
145568-byte physics object. The first time a bike pointer is seen, its stock
values are snapshotted; after that every write is `stock * multiplier`. That
makes the tick idempotent, so running it every frame never compounds, and
turning a section off writes the stock values straight back.

Tyres live inside the bike object, ten slots per wheel. Modifiers are applied to
every slot rather than just the active one, so changing tyres in the setup
screen does not drop the setting.

Crash tolerance is the odd one out: it is game code, not bike data. Two of the
four thresholds are constants referenced only by the crash check, so they are
overwritten in place. The other two are shared with unrelated code, so instead of
touching the constant the module repoints the instruction's RIP-relative
displacement at its own copy. Every site is byte-pattern checked first and the
patch is skipped entirely if the pattern does not match, which is also the
version check.

## Status

Builds clean as a self-contained x64 DLL:

- the full offset map, verified against the IDB
- the modifier engine for all five, with snapshot / restore
- the crash-tolerance code patcher, with byte-pattern verification
- the overlay, one tab per modifier plus a status tab
- the host: swap hook, window subclass, GL backend, injectable entry, clean unload

The overlay runs: it injects, hooks, initialises and draws in game. The five
modifiers themselves have still not been exercised — nothing has been enabled
and ridden yet.

## First crash, and what it changed

The first inject crashed the game instantly: `0xC0000005` in `mxbikes.exe` at
fault offset `0x2A79F6`, i.e. `0x1402A79F6`.

That address is inside `Net_QueryConnectionState` (`0x1402A79A0`), which the
offline guard used to call through the engine's dispatcher. The handler takes
**three** out-params, not two — the third is a string buffer it `strcpy`s a
status message into. We passed two, so `r8` held whatever happened to be in it
and the game wrote a string through a garbage pointer.

The fix is not a better-guessed signature. The same function reads a plain
global, `g_NetConnectionState`, so the guard now reads that directly: no
calling convention to get wrong, no side effects, nothing to call. The rule this
established for the rest of the module is that it reads and writes game memory
but never calls game code.

Everything up to that point did work — the hook fired, ImGui initialised, the
fonts and atlas built, the GL backend came up and the first frame reached
`Tick()`. The menu drawing path and the modifiers themselves are still untested.

## The freeze

After the first inject was fixed, opening the menu froze the game until INSERT
was pressed again.

The thread was not stuck: INSERT still worked, so messages were still being
dispatched. That rules out a hang in the drawing code and leaves frame
advancement, which this game gates on an empty message queue.

The subclass was swallowing keyboard and mouse messages with `return 1` while
the menu was open, so `DefWindowProcA` never ran for them — and `App_WndProc`
always runs it. Interfering with dispatch in a loop that only renders when the
queue drains is the wrong mechanism regardless of which message is to blame, so
the subclass now always forwards and input is taken away at the two collectors
instead. Auto-repeat on the INSERT key is also filtered now (lParam bit 30),
which it was not before.

## Second round of crashes

Three records, all from the Windows event log.

**`nvoglv64.dll`, `0xC0000005`, on track load.** A crash inside the NVIDIA GL
driver rather than in the game or in us. `glDrawElements` sources *every* enabled
client array using the index range it is given, including arrays the caller never
set up — and mxbikes.exe enables its own vertex, normal, texcoord and colour
arrays and uses multitexturing. Whatever it left enabled was being read through
*its* pointer with *our* indices: an out-of-bounds read inside the driver.

The same mechanism explains the invisible menu. `glTexCoordPointer` only affects
the selected client texture unit, so if the game left unit 1 selected, our UVs
went to unit 1 while unit 0 kept the game's stale pointer — textured geometry
could not sample the font atlas, while the untextured test quad drew fine. One
root cause, both symptoms.

The renderer now unbinds any buffer object and vertex array object (ReShade is
in this process and does use modern GL, where a bound buffer turns array
pointers into buffer offsets), disables every client array it does not use, and
resets the active client texture unit to 0. Everything past GL 1.1 is resolved
through `wglGetProcAddress` per context and treated as optional.

**`mxb_physics.dll_unloaded`, `0xC0000005` and `0xC000041D`, at offset
`0x7F50`.** Code ran in our pages after `FreeLibrary`. `Remove()` now detaches
the window subclass before touching the hooks, and refuses to report success if
the window proc is no longer ours — another subclass on top would still hold our
address, and restoring the original would not take us out of that chain. It also
refuses if any thread is still inside a callback after the drain. When detaching
cannot be completed the module stays resident and inert (hooks off, callbacks
pass-through) rather than unmapping under a live caller.

**GL context recreation.** Texture names belong to a context, and the game
imports `wglCreateContext` / `wglDeleteContext` / `wglShareLists`. The backend
now notices when the current context is not the one its objects were built on and
re-uploads the font atlas; the overlay re-uploads the background. `io.DisplaySize`
is also validated before `NewFrame`, since ImGui culls everything if it does not
believe it has a viewport and the value starts out as `(-1,-1)`.

## Diagnostics

The DLL writes `mxb_physics.log` next to itself on load, recording each hook,
overlay init, font pointers, background decode and shutdown. An injected overlay
has no console and a crash takes the process with it, so each line is flushed
immediately.

It also logs a heartbeat every two seconds with a frame count and the menu
state. Because frame advancement here depends on the message queue draining,
that line is the quickest way to tell a stalled loop (`frames` flat) from a
present or GL problem (`frames` climbing while the screen is static).

The status tab shows the live session state (`offline` / `connecting` /
`connected` / `unknown`) and whether writes are allowed.

## Porting the reference menu

Three things were lost in the first port of `skrt menu clone` and had to be put
back once the overlay was actually visible:

- **Slider spacing.** `InsertSlider` runs eight `Spacing()` calls before the
  control. That is not arbitrary padding — this fork draws a slider's label on
  the line *above* the bar, and that is the gap it needs. The port used one, so
  every slider overlapped the control above it.
- **Style restoration.** ImGui's style is persistent. The reference menu puts
  `ImGuiCol_Border`, `ItemSpacing` and `ImGuiCol_ChildBg` back before `End()`;
  the port dropped all three, so the values leaked into the next frame and the
  background child and group box borders drew with whatever the last spacer
  macro had set.
- **Tabs.** The reference used icon glyphs and the READ ME says they were not
  shipped, so the tabs carry short words instead. The recovered third font in
  `MenuFonts.h` is very likely that missing icon set, but its codepoints are
  unknown, so this stays text until someone maps them.

## Open questions

- The offline guard is settled. It reads `g_NetConnectionState` (RVA `0x3D7900`)
  directly: 0/1 offline, 2 connecting, 3/4 connected, anything else treated as
  online. See "First crash" below for why it is a plain read and not a call.
- Whether the suspension `SettingRange` values are re-read by the solver each
  step or baked once when the setup is applied. If they are baked, spring changes
  may need to land at a different point than once-after-load.
- **Input isolation covers the game's UI, not the bike.** The two collector
  hooks take window-message input away from the game's menus, but riding
  controls come in through DirectInput8
  (`IDirectInputDevice8::GetDeviceState`), which never touches a window proc.
  The bike can still be steered with the menu open. That needs a
  `GetDeviceState` hook.

## The empty bike list

`Bikes loaded: 0` had two causes, found one after the other.

First, modelling the engine's handle registry as a
C++ struct. Its entries begin 4 bytes into the block, but any struct containing a
pointer is 8-byte aligned, so `struct { int32_t count; RegEntry entries[]; }`
puts them at 8 — every bike pointer read 4 bytes off, came back as garbage, and
was rejected by the readability check. The count read fine, which is why it
looked like "no bikes" rather than a crash.

The walk now uses explicit byte offsets, and the status tab reports the pointer,
raw count, and how many entries were accepted versus rejected, so an empty list
can be told apart from an empty source.

That exposed the second cause: the registry was simply **null**. It holds bike
`.cfg` data for the setup and garage code, not running bikes. The live ones are
in per-world entity collections — `Physics_StepWorld` indexes `g_Worlds`
(RVA `0x5677A0`, stride 24928, worlds 1..3) and takes the collection at `+40` as
its bikes, passing each element straight into `Rider_Update`. So the element is
the bike physics object, and that is what the module now walks.

The lesson worth keeping: the config loader was the wrong thing to anchor to. The
physics step loop is the authority on where live objects are.

## Never crash

Implemented as the tolerance patch with an unreachable value (`1e24`), **not** as
a patch on `Rider_Detach`.

The first attempt did patch `Rider_Detach`'s entry to `mov eax, 1; ret`, and it
genuinely stopped the rider coming off — but every landing either launched the
bike into the air or killed the game. The call site is why:

```c
if ( fabs(sumLat) > 4000.0 || fabs(sumVert) > 10000.0 || ... )
{
    Rider_Detach(a1);
    return 1;          // <-- returns out of Rider_Update
}
// the rest of the rider physics never runs
```

Neutering the function does nothing about that `return 1`. On any frame where
the thresholds are exceeded, the rider simply stops being simulated: nothing
opposes the impact, so the bike gets flung, and far enough wrong the position
goes NaN and faults the terrain sampler.

Raising the thresholds instead means that condition is never true and
`Rider_Update` gets past it. `4000 * 1e24` is `4e27`, and the largest of the four
lands at `1.5e28` in a float against a `3.4e38` limit, so nothing overflows to
infinity.

That alone was still not enough, because `Rider_Update` has **three** exits that
each do `Rider_Detach(a1); return 1;`:

| site | condition | handled |
|---|---|---|
| `0x1401AD30E` | the four force thresholds | `Apply()` scales the constants |
| `0x1401A85C4` | rider at or below the ground | **deliberately not suppressed** |
| `0x1401A8799` | lean angle past 75 degrees | opcode patch |

The lean limit's `75.0` is shared with three unrelated functions (one of which
divides by it), so the constant cannot be touched. Its "skip the detach"
conditional jump is made unconditional instead — `jbe` `0x76` becomes `0xEB`,
one opcode byte, verified before writing and restored on undo. The rel8
displacement is untouched so the jump still lands where the original did.

### Why the ground-contact exit is left alone

It looks like a third crash rule and is not one. `Rider_GroundQuery` fills a
surface-contact record at `bike+144652`, and the test is "the rider's reference
point is at or below that surface". Detaching is the **only** response the solver
models for a rider inside the terrain — there is no attached-and-intersecting
state. Forcing the skip leaves the rider embedded and something downstream
ejects it, which presented as the bike in the air with the camera under the
ground after a gentle landing over a berm, then a NaN.

So putting your body in the dirt still ends the run with Never crash on. That is
what **Auto-recover** is for.

## Auto-recover

Lets the crash happen, then puts the rider back on. It is the only thing that
covers body-to-dirt contact, since that exit cannot be suppressed.

`Rider_Reset` (`0x1401957D0`) is the game's own routine for this: it clears the
crashed flag and rider state, re-seats the rider transform from the bike's, and
clears the rider velocity and history buffers — all without moving the bike.
`Bike_PlaceOnTrack` calls it with nothing but the bike pointer, which is why it
works standalone.

A configurable delay (default 0.5s) means the rider spends a moment on the floor
rather than snapping back mid-tumble; 0 is immediate.

### On calling game code

This module's rule is to read and write game memory but never *call* it, set
after a guessed three-argument signature wrote a string through a garbage
pointer and killed the game on the very first inject. This call is a deliberate
exception, and a materially different situation: the signature is read straight
off the disassembly (one pointer in `rcx`, no float arguments, `int` return), it
is invoked exactly as the game invokes it at `0x140197A45`, and it runs on the
same thread, since the swap hook and the physics both live on the main thread.
Everything it touches is inside the bike object except one pointer at
`bike+144512` that it dereferences, which is validated first.

## Writes only happen when a multiplier is not 1.0

Every Apply now skips a field whose multiplier is exactly 1.0, because
`stock * 1.0` is the value already there. With all five sections enabled by
default this means a freshly injected build touches nothing until a slider
moves, which makes it far easier to attribute a crash, and keeps the module out
of the game's memory when it has nothing to contribute.

Engine curves are also re-validated on every write rather than trusting a
pointer captured earlier: `Curve_LoadFromCfg` stamps `type = 12`, the point
count has to match the snapshot, and the array has to still be readable. The
points live on the config heap, which is exactly the memory most likely to have
been reused.

## The NaN crash

Two identical faults at `mxbikes.exe+0x1F1923`, inside a terrain heightmap
sampler: `movsx ecx, word ptr [r9+rax*2]`, where the index is built from the
bike's position. An out-of-range index there is the signature of a NaN reaching
the physics — NaN converted to an integer is `INT_MIN`, which sails straight past
the bounds clamp a few instructions above and faults on the read.

Every value the module writes now goes through one clamp helper that rejects
non-finite results and holds each field inside a sane range, writing the stock
value instead of a degenerate one. The fields that matter most are the ones used
as denominators, because a zero there produces the NaN in the first place:

- spring rates and tyre peak friction (non-positive breaks the solver)
- driveline efficiency (clamped to 0.01..1.0) and axis inertias
- gearbox teeth and sprocket teeth (a zero is a divide-by-zero in the driveline)
- shift and clutch times

Worth noting the slider ranges alone could not produce a zero, so the clamps are
belt and braces for the arithmetic rather than a fix for a known bad value. The
other candidate for this crash is **Never crash** itself: with the detach check
bypassed, a bike that would normally have ended the run can keep going until it
leaves the heightmap entirely, which produces exactly this fault without any NaN
involved. Testing with sliders moved and Never crash *off* separates the two.

The heartbeat line now lists every modifier that is not at its neutral value, so
the last entry before a crash says what was actually active.

## GL errors the backend was leaving behind

The log showed `glGetError 0x0501` (`GL_INVALID_VALUE`) and `0x0500`
(`GL_INVALID_ENUM`) after every frame we drew. Both were ours:

- The enum errors came from issuing tokens newer than the context: `GL_MULTISAMPLE`
  (1.3), `GL_SECONDARY_COLOR_ARRAY` / `GL_FOG_COORD_ARRAY` (1.4),
  `GL_MAX_TEXTURE_UNITS` (1.3) and `GL_FRAMEBUFFER_SRGB` (3.0). The backend now
  parses `GL_VERSION` once per context and only touches what that version knows.
- The value error came from `glScissor` being handed a negative width or height
  when a clip rect was inverted. Those commands are skipped now.

This is not cosmetic: mxbikes.exe imports `glGetError` and checks it itself, so
leaving our errors queued hands the game a failure that has nothing to do with
its own rendering. The backend now drains the queue on the way in (so the error
it reports is unambiguously ours) and on the way out (so the game never sees it).

## Visuals

### Watermark

Reproduced from `solus.lua`'s `renderer_shit` helper, which is the gamesense
indicator-bar style. The geometry is exact:

| reference call | what it draws |
|---|---|
| `circle_outline(x+5, y+8, 8, 180, 0.25, 2)` | top-left rounded corner, radius 8 |
| `circle_outline(x+w-5, y+8, 8, 270, 0.25, 2)` | top-right rounded corner |
| `rectangle(x+5, y, w-10, 2)` | the 2px accent rail between them |
| `gradient(x-3, y+8, 2, 6, a -> 0)` | left tail, fading downward |
| `gradient(x+w+1, y+8, 2, 6, a -> 0)` | right tail |

Radius 8 centred at `y+8` puts both arc crowns exactly on `y`, which is why the
rail lines up with them — worth knowing before changing any of those numbers.
Layout matches too: `h = 18`, `w = text width + 8`, text at `(x+4, y+4)`,
positioned `screen_width - w - 10` and `10 + 25*index` so further indicators can
stack beneath it. Default accent is gamesense's own `142, 165, 229`.

Branding is `nemora`. It draws on ImGui's foreground draw list, so it shows with
the menu closed — which is also why anti-aliasing needs setting explicitly: that
list takes its flags from the shared data at `NewFrame` and the arcs came out
aliased, so `DrawIndicatorBar` sets `AntiAliasedLines`/`AntiAliasedFill` itself
and restores the previous flags afterwards.

### FOV override

Two wrong levers before the right one.

**The camera table.** `Camera_LoadCfg` holds `camera%d/fov`, but the render path
never reads it, so writing it changed nothing.

**The `glFrustum` hook.** `Render_MainView` passes precomputed frustum planes
straight to `glFrustum`, so rescaling them there did change the picture — and
broke the periphery at high FOV. Two reasons, both found by reading
`View_BuildFrustum` (`0x140249510`) properly:

1. **Culling was left behind.** From the FOV at `camera+4`, that function derives
   the near-plane extents *and* the far-plane extents, the screen projection
   matrix, the eight frustum corners and the **six culling planes** at
   `view+304`. Rescaling the planes downstream widened the projection and
   nothing else, so geometry only visible in the newly exposed edges was still
   being culled against the stock frustum.
2. **Two of three passes were never touched.** Three `glFrustum` calls take the
   player's view object — one in `Render_MainView`, two in `Render_ViewPasses`
   (`0x14024FAD9`, `0x14024FF02`). The return-address filter scoped to
   `Render_MainView` caught only the first, so the other two kept projecting at
   the stock FOV while the rest of the frame was wide.

So the override now hooks `View_BuildFrustum` and overrides `camera+4` for the
duration of the call, which keeps projection, far extents, corners and culling
consistent because they are all computed from that one number. The camera object
is shared across views and frames, so the stock value is put straight back.

No call-site filtering is needed: every other kind of view excludes itself,
because the branch it takes never reads `camera+4`. Reflections set
`view+192 == 1`, explicit-bounds views set it to 2, ortho sets `view+196`, and
`Render_CubeProbe`'s six faces set `view+188` and `view+400` (they carry their
own 90-degree FOV). A plausible FOV in `camera+4` doubles as the check that the
pointer really is a camera.

The slider is in the game's own units — degrees referenced to 4:3, the same
number a `cameras.cfg` entry carries. `View_BuildFrustum` widens it by
`aspect/1.3333` on anything wider than 4:3, so 75 here is not 75 degrees on a
16:9 screen. The menu shows the stock value beside it.

One artifact is the engine's own and stays: `near = tan(10 deg) / tan(fov/2) * view+104`,
so the near plane moves closer as FOV rises — about 4.9x closer at 150 than at
75 — which costs depth-buffer precision in the distance. Diverging from that
formula would desynchronise the near plane from everything else derived with it.

### World modulation

Night mode, fog and tinting, on its own `WORLD` tab. Five interceptors, each
rewriting a value **as the engine applies it** rather than writing the global
behind it — those globals are recomputed every frame from weather and time of
day, so a write would be overwritten, and an interception needs nothing saved or
restored. Switching the modifier off restores the stock look on the next frame.

| hook | drives |
|---|---|
| `GL_SetClearColor` `0x14022FB50` | clear colour, *behind* the sky |
| `GL_SetFogColor` `0x140228CF0` | fog colour |
| `GL_SetFogEnable` `0x140228C90` | fog on/off |
| `opengl32!glFogf` (`GL_FOG_DENSITY`) | fog density |
| `opengl32!glLightfv` (`GL_LIGHT0`, `GL_AMBIENT`) | world light level |

That last row is a trap worth recording. `Render_MainView` calls
`glLightfv(0x4000, 0x1200, ...)`, and **`0x1200` is `GL_AMBIENT`, not
`GL_DIFFUSE`** (which is `0x1201`). Since `glLightModelfv(GL_LIGHT_MODEL_AMBIENT,
...)` is hard-coded to `{0,0,0,1}`, `GL_LIGHT0`'s *ambient* term is where this
engine keeps the world's light level. The first version of this hook filtered on
`GL_DIFFUSE` and therefore never fired once; it was caught by checking the
constant against `GL.h` rather than by testing.

Night mode is a blend on top of the manual knobs rather than a preset that
overwrites them, so turning it off puts everything back without clobbering
anything set by hand.

**What this does not reach: the sky.** `GL_SetClearColor` is `glClearColor`, the
colour the framebuffer is cleared to, and tracks draw a sky dome over the whole
view — so it is normally invisible and is *not* a skybox tint. The control is
labelled "clear colour" for that reason. What was mistaken for a sky tint is the
light tint, which multiplies `GL_LIGHT0`'s ambient and so shifts the hue of
everything the light touches. Tinting the sky itself needs the sky draw path,
which is the same unfinished work as chams.

The screen filter is a flat veil in the background draw list. The lighting
interceptors only reach what the light touches, so the veil is what catches the
sky texture and anything unlit — the difference between "the world is darker" and
"it is night". It gets its own colour rather than reusing the light tint, because
ImGui draw lists blend with straight alpha and a neutral white tint used as a
veil would wash the screen out instead of darkening it.

### China hat

Real 3D geometry, not a flat screen-space triangle: a world-space ring plus an
apex, each point pushed through the engine's own screen matrix, so it holds
perspective and leans correctly as the camera moves. Foreground draw list with no
depth test, so it shows through terrain — the whole point of the thing.

`Mat_TransformPoint` (`0x140266D80`) settles the matrix *layout* for the whole
engine: **row-major, translation at float index 3/7/11**, not GL's column-major
12/13/14.

What it does not settle is **direction**, and that cost a test round. The first
version read `bike+928`'s translation as the bike's position. It is the *inverse*
transform, so the translation is `-R*pos` — right magnitude, scrambled axes — and
the hat rendered at a fixed point far outside the map. The engine stores
inverses freely: `View_BuildFrustum` opens with `invert(camera+32, camera+96)`
(dst first), so `camera+96` is the camera's placement transform and `camera+32`
is the view matrix derived from it.

The fix is to stop guessing and use `bike+75720`, a plain world-space `float[3]`
(`+ kWheelStride` for wheel 1). `Bike_BuildStateSnapshot` takes the midpoint of
the pair and writes it straight into the translation slots of a local-to-world
frame, then inverts that frame to express the world centre of mass inside it —
which confirms the layout, the space and the direction all at once. That midpoint
is the engine's own reference point for a bike, so it is the right anchor
anyway; it just sits at wheel level rather than at the bike's origin, so the
height slider is measured from there. World up is `+Y`.

And `view+116` is the engine's *own* world-to-window matrix. Push a world point
through it with the convention above, divide x and y by z, and both come out as
fractions of the render target — x left to right, y **bottom** to top, GL window
convention. That was derived by evaluating the matrix at the frustum edges, which
means it holds regardless of whether `camera+32` is local-to-world or its
inverse: no handedness guess, and no GL state work at all.

The view is snapshotted in `EndViewFrustum`, *after* the original has run,
because the screen matrix does not exist until then. A side effect worth having:
with the FOV override on, the hat is projected through the overridden frustum, so
it stays consistent with what was drawn.

Styles: solid, rainbow (whole hat cycles), pulse (brightness breathes), and hue
sweep (hue runs around the cone, so it reads as spinning). The hat is rejected
whole if any point falls behind the near plane — at that distance the rider fills
the screen and a half-clipped cone just flickers.

### Auto gesture

Pick the gesture from a dropdown — raise arm `M`, complaint `N`, angry `H`, chat
`T`, trick `G`, matching the game's control defaults. Win gesture is on XInput
D-pad right with no keyboard binding, so it is not offered. Delivered with
`SendInput`, which injects below both the window-message and DirectInput paths.

The trigger dropdown has **On crash**, which works, and **On overtake**, which
does not yet. Overtake needs the race position, and the HUD path
(`sub_1400BF610`, the `ID_POSITION` widget) reads it out of session state at
`qword_14050FEC0` rather than from the bike, so it still needs tracing. The
dropdown entry says so rather than silently doing nothing.

## Chams

Recolours the rider and the bike by overriding their material, on its own
`CHAMS` tab. Materials: flat, glow, rainbow, metallic, pearl, each with a colour,
separately for rider and bike.

### Where it hooks, and why the first version drew nothing

`Mat_SetupMaterial` (`0x14024B6E0`) carries every `glMaterialfv` in the
renderer, so it looked like a pre-draw state setter worth overriding *after*.
Nothing appeared on screen.

It is not a state setter — **it is the draw call**. The giveaway is in its own
body: it increments per-draw statistics as it runs, including a texture-switch
counter (`if (dword_140399098 != v17) ++dword_1403990A0`) and a `realloc`-grown
table of unique texture ids. Bookkeeping like that belongs to something issuing
a draw, not to something preparing one. By the time the hook regained control,
the mesh was already rendered, and the material was being set for nobody.

So the game's own material calls are now rewritten **as it makes them**:

1. `ChamsBegin` decides whether this mesh is a target and resolves its colours
2. the original runs, and every `glMaterialfv`, `glMaterialf` and
   `glBindTexture` it issues passes through a GL hook that substitutes
3. `ChamsEnd` disarms

Nothing is written behind the renderer's back, and the timing cannot be wrong
because the substitution happens inside the call that draws.

The colours are resolved once per mesh in `ChamsBegin` rather than per
`glMaterialfv`, since the renderer issues several of those per draw.

### What a1 actually is, settled from the disassembly

Two rounds were spent guessing at `Mat_SetupMaterial`'s first argument, so it is
worth writing down what it is.

`a1` is **not** the entity — every call site builds a local struct and passes its
address (`lea rcx, [rsp+var_D8]` in `Render_RiderObject`, `mov rcx, rsi` in
`Render_SceneModels`, where `rsi` is a node from the per-frame draw list at
`qword_14057CB58`).

But `*a1` **is** the entity, in both paths:

- `Render_RiderObject` writes `mov [var_D8], r15`, and its prologue has
  `mov r15, rdx` — the second argument, which `Render_MainView` passes as the
  scene entity.
- `Render_SceneModels` does `mov rbx, [rsi]` and then reads light colour from
  `[rbx+27Ch]`, `[rbx+280h]`, `[rbx+284h]` — the exact fields
  `Render_RiderObject` reads off its entity argument.

The collection layout the vehicle list depends on is confirmed too:
`Collection_At` returns `*(_QWORD *)&a1[4*i + 3]`, i.e. the pointer at byte
`16*i + 12`, which is what `Process.h` already encodes.

### Why it kept colouring the track

Twice. Both times the same root cause: using a *part* of the game's own test
instead of all of it.

`entity+268 == 1` looked like the vehicle test because `Render_MainView` uses it
to pick out vehicles. It is not. **Type 1 does not mean "vehicle"** — scenery and
props are type 1 as well. The game's actual predicate was sitting in
`Render_ViewPasses` the whole time, and it has three parts:

    if (*(_DWORD *)(v8 + 268) == 1          // type
        && *(_QWORD *)(v8 + 6392)           // owns a rider object
        && !_bittest((v8 + 272), 8u))       // not hidden

What separates a vehicle is **owning a rider object at +6392**, which scenery
leaves null. The first attempt used only the type, which matched the whole
world. The second attempt kept the same one-third test and merely moved it into
a per-frame list, so it matched the whole world just as accurately.

`RefreshVehicles` now copies that predicate whole. The lesson is narrower than
"check more things": when the engine already contains the test you are trying to
reproduce, copy all of it, because the parts you leave out are exactly the parts
that make it selective.

Every path checks the list, including the rider. `Render_RiderObject` is only
ever called for a vehicle's `+6392` object so its return address would be
identification enough — but an earlier version used that to exempt the rider,
leaving one route able to colour anything if the range were ever wrong. Given
how this particular failure kept recurring, making it impossible by construction
beats arguing that it cannot happen.

The status line reports all three stages — `entities N, type1 M, vehicles K` —
precisely so that "the filter is too loose" is visible at a glance rather than
inferred from what is on screen.

### Why fixed-function material still works

Because the game's GLSL reads `gl_FrontMaterial`. Correcting a claim made
earlier in this project and then half-corrected again: the renderer is **not**
pure fixed-function — it ships `#version 110` shaders loaded through
`wglGetProcAddress`, which is why no shader entry point appears in the import
table — but its **materials are** fixed-function. Setting `glMaterialfv` reaches
the shader.

### Telling the rider from the bike

Not by inspecting meshes. `Mat_SetupMaterial` has exactly two callers, and that
is the whole classifier:

| caller | draws |
|---|---|
| `Render_SceneModels` `0x14024E740` | track, bikes, props |
| `Render_RiderObject` `0x140261220` | **only** a type-1 entity's `+6392` object |

`Render_MainView` calls the second one from inside its entity loop, guarded by
`entity+268 == 1`, so a return address inside `Render_RiderObject` means the
rider and anything else means the ordinary model. The `+6392` object exists only
on vehicles and gets its own renderer, which is what an animated character needs
and an ordinary model does not.

That inference is good but not proven, so there is a **Swap rider / bike**
checkbox. Unlike the old car-mode invert, this is cosmetic — nothing here can
reach the solver — so a toggle is the right answer rather than a risk.

The entity type is checked too (`entity+268 == 1`, vehicles only). Without it the
track takes the material as well, which is a wallhack rather than chams.

### The first version tanked the frame rate

`Mat_SetupMaterial` runs once per **mesh** — hundreds of times a frame — and the
first version called `IsReadable` twice inside it. `IsReadable` is a
`VirtualQuery`, so that was two kernel transitions per mesh, and it cost most of
the frame rate.

They were not even buying anything. The original function has already
dereferenced `*a1` and read `entity+268` before the hook regains control, so
those reads are proven safe by construction. The validation that is right in a
once-a-frame tick is wrong in a per-mesh one.

The rule now sits in a comment at the top of both the hook and `Chams.cpp`:
nothing on this path may do a syscall, allocate, or take a lock. The
return-address bounds are computed once into statics for the same reason.

### Why material alone was not enough

The first version only set `glMaterialfv`, and nothing visibly changed. Material
state loses to a bound GLSL program sampling a texture: the shader multiplies the
colour by the bike's skin and the skin wins.

**Force flat** (on by default) unbinds the texture for a chammed mesh, through
the `glBindTexture` hook. It is scoped to the armed window, so it affects
exactly the meshes being chammed and nothing else — no save-and-restore dance
is needed, unlike the earlier attempt that unbound the shader program and had to
put it back at the top of the next call.

### Reading the Status box

It reports total meshes seen, how many were vehicles, how many were recoloured,
the GLSL program that was bound, and whether `glUseProgram` resolved. That
distinguishes the failure modes without guessing:

| reading | meaning |
|---|---|
| a hook reported MISSING | that hook did not install; nothing else matters |
| total 0 | `Mat_SetupMaterial` never fired |
| total high, vehicle 0 | the entity type check is wrong |
| vehicle high, chammed 0 | the rider/bike split is wrong — try Swap |
| chammed high, material writes 0 | the mesh never sets a material we substitute |
| material writes high, nothing visible | the shader is ignoring `gl_FrontMaterial` |

The last row is the only one that would mean this approach is wrong rather than
mis-wired, and it is the reason the material and texture hooks are reported
separately: an armed substitution that never fires looks identical to no chams
at all from the outside.

### What it does not do yet

**Local-player-only filtering.** The pieces are found — `Profile_ApplyAidsToRider`
indexes a slot array at `g_PlayerSlots` (`0x140F4EE20`, stride 23332) by
`g_LocalPlayerIndex - 1` and reads the rider id at `slot+4` — but whether that id
shares a namespace with the scene entity collection's handles is not confirmed,
and shipping a filter that silently fails to filter is worse than not shipping
one. Chams currently apply to every vehicle. NOTES.md keeps this mod
singleplayer-only, so in practice that is the player, plus AI when a race has
any.

The Status box reports how many rider and bike meshes were recoloured last
frame, so "nothing happened" can be told apart from "the hook never fired".

### Still not wired

**Skybox replacement** is still config-side only. The keys are located:
`clear/sky_color/*`, `clear/sky_rot`, `clear/sky_height`, `sun_position/{x,y,z}`,
with parallel `cloudy/*` and `rainy/*` sets. Tinting the sky is already covered
by the world modifier; swapping the texture is not.

**Overtake** still needs the race position — see the auto gesture section.

## Field probe

A field finder on the `INFO` tab, for locating something in the 145 KB bike
object without static analysis. Guessing offsets is how this project produced
its NaN crash and parked the china hat outside the map, so this is the
alternative to guessing when IDA is not to hand.

It is also a better instrument than static analysis for one specific job:
identifying a **control axis**. Code tells you where a value is written; only the
running game tells you that a field sits at -1, then 0, then +1 as you move a
stick.

Capture the whole bike object under deliberately opposite conditions, then ask
which fields can only be the thing you are after. Matches go to the log rather
than the menu, because there can be dozens and they need reading side by side.

| mode | matches |
|---|---|
| `Control axis (f)` | float within +/-1.5 that swapped sign between A and B |
| `Control axis 3pt` | the same, and C sits within +/-0.1 of centre |
| `Sign flip (f)` / `(d)` | any float/double that swapped sign — rates, angles |
| `Changed (f)` / `(d)` | moved substantially |
| `Unchanged (f)` | did not move at all, for finding constants |

### Finding a control axis

1. Hold the input at one extreme, **Capture A**.
2. Release to centre, **Capture C**.
3. Hold the other extreme, **Capture B**.
4. Mode `Control axis 3pt`, **Compare to log**.

A centred -1..+1 control is then nearly the only thing that can match: it has to
be in range, straddle zero with both ends clear of it, *and* return to centre.

Captures must come from the same bike pointer; comparing two different bikes
would report every field that differs between two vehicles, so a mismatch is
refused rather than reported. Fields are read at their natural alignment (4-byte
stride for floats, 8 for doubles) — reading at every byte offset finds mostly
noise. Results are capped at 70 lines, and the log says when there were more, as
a hint to tighten the capture conditions rather than to read a thousand matches.

## Car mode

Steering rotates the bike directly instead of leaning it: the steer input drives
the chassis body's **yaw rate**, the horizontal velocity is rotated by the same
angle so the bike goes where it points, and roll and pitch rates are bled off so
it stays upright.

### Two attempts that were wrong

**Invert steering.** The first working version offered an invert toggle, because
world handedness is not readable from the exe. Ticking it killed the game. Not a
code bug: yawing *against* the steering geometry holds the tyres at roughly twice
the steer angle of slip, permanently, and the tyre model diverges into a NaN that
poisons the whole solve. The default direction tested correct, so the option had
no remaining purpose and is gone — the sign is fixed.

That crash is also what prompted the divergence guard below, since the same
blow-up is reachable by other means (a high yaw rate against low grip, say).

### The aids attempt was wrong too

It was built out of the game's own rider aids -- `stabilityhelp` cranked to keep
the bike upright, `leanhelp` to supply the turn, `directsteer` mode 2 for bars by
angle. It made the bike **harder** to turn, and sometimes killed the game on a
wall impact.

The reason is simple once stated: **a bike turns BY rolling.** A self-balancing
assist resists roll, so cranking it removes the turning mechanism, and
`leanhelp` was then fighting the very assist that was supposed to be holding the
bike up. The two cancelled. Gains at 4.0 also meant enormous corrective torques
on a hard impact, which is where the crashes came from.

Worth recording as a reasoning error rather than a coding one: the aids were
real and the writes landed, but "upright" and "turning" are the same axis on a
motorcycle, so turning one up necessarily turns the other down.

### What it does now

Per frame, for each loaded bike:

    steer   = *(double*)(bike + 720)         // active input copy, -1..+1
    chassis = *(void**)(bike + 1072)
    ramp    = clamp(horizontalSpeed / speedRef, 0, 1)
    yaw     = steer * yawRate * ramp

    angVel[1]  = yaw                          // +Y is up, so this is yaw
    angVel[0] *= (1 - upright)                // bleed off roll
    angVel[2] *= (1 - upright)                // and pitch

    // steer the velocity onto the bike's own heading, at the same speed
    linVel.xz += (forward.xz * speed - linVel.xz) * grip

The speed ramp is what makes it feel like a car rather than a turntable: at a
standstill there is no yaw, and the rate fades in, so what you get is a turn
radius.

### Fixing the residual slide

The first version rotated velocity by `yaw * dt`, reasoning that the body would
turn through exactly that angle. It slid anyway, and the reason is that the
solver *changes* the angular velocity between our once-per-frame writes — tyre
forces, gyroscopic effects — so the body never rotates by exactly `yaw * dt` and
the velocity drifts out of line with it.

Aiming at the body's **measured** heading instead removes the dependence on the
interval altogether, and makes `grip` mean something honest: 1.0 is on rails, 0
leaves the stock sliding alone.

That needs to know which body axis is forward. It is **not assumed** — guessing
a matrix convention is what put the china hat outside the map. `Body_AddLocalTorque`
computes `out = (v·(f52,f53,f54), v·(f56,f57,f58), v·(f60,f61,f62))`, mapping
local to world with those as *rows*, so the local axes are the matrix's
**columns**, strided by 4 floats. Which column is forward is then resolved at
runtime by watching the bike ride straight and quickly (`speed > 5 m/s`,
`|steer| < 0.08`) and taking the column whose horizontal projection matches the
direction of travel with `|dot| > 0.95`. A marginal winner is rejected and a
better sample waited for. The result is latched and logged, and the menu shows
which axis won.

### The divergence guard

Every value written is saved first, and if the result comes back non-finite, over
200 m/s, or over 50 rad/s, the write is undone rather than left in the solver.
Those bounds are not handling limits — 200 m/s is 720 km/h — they exist purely so
a diverging solve cannot become a crash. Rejections are counted and shown in the
menu.

### Why this is safe to write

The physics library's own `Body_SetAngVel` (`0x1402AC890`) is three plain float
writes to `body+288` with no side effects, so setting a velocity is a supported
operation that can be replicated with memory writes. Nothing calls into game
code.

Working at the velocity level also matters: the solver integrates the result
instead of being contradicted by it. Writing the transform directly is the same
shape of mistake that made never-crash fling the bike across the map.

A frame gap over 100 ms (paused, loading, alt-tabbed) also skips the tick, since
acting on a stale interval is how never-crash flung the bike.

The tick runs on the render frame rather than from a `Bike_Update` hook. It only
reads and writes memory, so it needs no new hook and no guess at that function's
calling convention, and an angular velocity persists until the solver changes it
-- so once a frame is enough.

### The rigid body

Mapped from the accessor family around `0x1402AC8B0`, every one of which does
nothing but return an offset or write three floats:

| accessor | offset | meaning |
|---|---|---|
| `Body_GetPosition` / `Body_SetPosition` | `+192` | position |
| `Body_GetOrientation` | `+208` | orientation, 3 rows of 4 floats |
| `Body_GetLinVel` / `Body_SetLinVel` | `+272` | linear velocity |
| `Body_GetAngVel` / `Body_SetAngVel` | `+288` | angular velocity |
| `Body_AddWorldForce` | `+304` | force accumulator |
| `Body_AddWorldTorque` / `Body_AddLocalTorque` | `+320` | torque accumulator |

A bike owns three: chassis `bike+1072`, fork `bike+14208`, rider `bike+144512`.
`Bike_BuildStateSnapshot` accumulates the centre of mass over exactly those
three and skips the rider when crashed, which is what identifies the set.

### The control inputs

`Bike_Update` (`0x1401ADA40`) latches the rider's controls as its first act: the
raw block at `bike+624` is copied into the active block at `bike+720`, and when
the rider has crashed or is in the pit the active block is **zeroed**. That
zeroing is the proof -- a crashed rider cannot steer.

The order matches `g_ControlTable` exactly, and the two `int`s land precisely
where `CTRL_SHIFTUP` and `CTRL_SHIFTDOWN` sit, which pins the mapping:

| offset | control |
|---|---|
| **+624 double** | **`CTRL_LEAN` — the steering axis, −1..+1** |
| +632 / +640 / +648 / +656 | throttle / brake / rear brake / clutch |
| +664 / +668 (int) | shift up / shift down |
| +672 / +680 | fwd-back lean / left-right lean |

Car mode reads the **active** copy, not the raw one, for two reasons: it is what
the physics actually used, and it comes pre-zeroed while crashed, so a crashed
rider stops steering without needing a check.

## Horsepower and top speed

Two engine modifiers that are genuinely distinct from the torque multiplier.

**Horsepower (revs).** Power is torque times angular velocity, so letting the
engine rev further raises peak power without changing the torque it makes at any
given rpm — which is exactly the difference between tuning for torque and tuning
for horsepower. It scales `engine/MaxRPM` (`bike+3648`), `engine/ShiftRPM`
(`bike+3652`) and every `engine/Limiter` (`bike+1992`, `NumLimiter` doubles)
**together**. Raising the ceiling and leaving the limiter alone would just mean
hitting the limiter, which looks identical to the feature not working.

Those RPM fields are divided by 60 immediately after loading, so they hold
revolutions per second rather than RPM. Irrelevant to a multiplier, but not to
anything that prints them.

**Top speed.** Through aerodynamic drag: `Aero/MinCdA` (`bike+145576`) and
`Aero/MaxCdA` (`bike+145584`), doubles, min being the tucked pose and max
sitting up. CdA is the drag coefficient already multiplied by frontal area, so it
is the one number that sets terminal velocity.

It is applied as `stock / (mult * mult)`, not `stock / mult`, because drag goes
as `CdA * v^2` and so top speed goes as `1 / sqrt(CdA)`. Squaring makes the
number on the slider the speed multiple it is actually worth.

This only bites while the bike is **drag-limited**. A motocross bike that revs
out in top gear is gear-limited instead, and then cutting drag does nothing —
raise the rev range or the gearing. The tab says so.

### The stale-value bug these exposed

Adding the two new modifiers made the bike behave erratically, and the cause was
a bug in how the engine section decided when to write:

    if (rev != 1.0f || !on) { ...write... }

Drag a slider **back to exactly 1.0** and that stops writing, which leaves the
bike on the last scaled values indefinitely. "The multiplier is 1" and "the bike
has been put back" are not the same state, and the condition conflated them.
Only toggling the whole section off ever restored anything, so the symptom was
stale extreme values persisting in combinations never actually set.

Both the engine and transmission sections now keep a `dirty` flag and write once
more after everything returns to neutral, so the bike genuinely lands back on
its own numbers. Grip and suspension were already safe — they write through
`on ? scaled : stock` ternaries, which always write.

### Guarding the raw scalars

Unlike the torque curves, the RPM, limiter and CdA fields are bare scalars with
no pointer to validate, so a snapshot taken before the cfg finished loading
would be multiplied into the live engine every frame. They are now plausibility
checked at capture — revolutions/second between 1 and 3000, CdA under 5 — and skipped
entirely if they fail, with the captured values logged once per bike so a bad
snapshot is visible rather than inferred.

The rev range is also capped at 3x rather than 10x. Past that the crank's
gyroscopic term and the driveline stiffness genuinely exceed what a fixed-step
solver integrates cleanly, which is a physical limit rather than a bug to fix.

### Bike_LoadCfg is the wrong place to read live offsets from

This cost a test round and is the more important of the two lessons here.

Every offset for horsepower and top speed was read out of `Bike_LoadCfg`
(`0x140168CD0`) by following `lea rdx, <dest>` / `lea r8, <key>` / `call
Cfg_GetX`. The reads were accurate. They were also **useless**, because that
loader parses into a *different struct layout* from the live bike object — the
engine fields sit 264 bytes further on and the aero fields 232, so writing to
them hit whatever happens to live at those addresses in a running bike. That,
not the stale-value bug found alongside it, is why the bike went haywire.

The live layout is confirmed instead by `0x14018FF80`, `0x1401B5B30` and a
running bike, and `Offsets.h` now carries that warning at the top of the engine
block.

**The cross-check that should have caught it was circular.** The derivation
looked trustworthy because `engine/Clutch/MaxTorque` came out at 3240, matching
what `Offsets.h` already recorded — but that recorded value had come from the
same loader, so the agreement confirmed nothing except that the mistake was
consistent. Its real live offset is 3504. A cross-check is only worth something
when the two sides were derived independently.

The live values prove themselves properly: `maxRPM=195.8 shiftRPM=161.7`, which
at 60 rev/s is 11,748 rpm with a 9,702 rpm shift point, and `cdA=0.41/0.46`.
Those are what a 450 should look like, and nothing in the loader could have told
us that.

There is a second, smaller trap in that loader worth recording for anyone who
does read it for other reasons: **the two leas appear in either order** — the
Aero block emits the destination first, the engine block emits the key first —
so taking the nearest preceding `lea rdx` returns the *previous* key's
destination half the time. Read the pair that brackets each call.

## Testing build switch

`MXB_TESTING_NO_GUARDS` in `src/mxb/Modifiers.h` is currently **1**. That:

- disables the offline guard, so the modifiers write whatever the session is
- turns every section on by default, so nothing has to be enabled by hand

It exists to get the physics path exercised without fighting the safety rails.
The guard is the thing that keeps this module singleplayer-only, so **set it back
to 0 before this is used for real**. The status tab says plainly when a build has
it enabled.

## Scope

Offline play and the user's private-server testing. The stock verification
response behavior and its limits are documented above.
