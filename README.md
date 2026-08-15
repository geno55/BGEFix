# BGE Alt+Tab Fix

Restores **Alt+Tab** in the GOG.com release of *Beyond Good & Evil* (2003), without
throwing away the compatibility fix you actually want, and with no third-party downloads.

---

## The problem

Alt+Tab does nothing in the GOG release. The game does not crash, does not minimise —
the keystroke simply vanishes.

This is **not** a DirectX bug, a driver bug, or a fullscreen-focus bug, which is why the
usual advice (DxWnd, dgVoodoo2, DXVK, borderless-window utilities) mostly fails or makes
things worse. Alt+Tab is *deliberately disabled*.

The GOG installer ships `goggame.sdb` in the game folder and registers it as a
machine-scope **Windows Application Compatibility shim database**:

```
C:\Windows\AppPatch\CustomSDB\{a0619476-ee14-4631-b5e4-36bcb2c6e987}.sdb
  description : "GOG.com Beyond Good and Evil"
  registered  : HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Custom\BGE.exe
```

It applies exactly two shims to `BGE.exe`:

| Shim | Effect | Verdict |
|---|---|---|
| `IgnoreAltTab` | Makes the game swallow Alt+Tab entirely | **This is the bug** |
| `SingleProcAffinity` | Pins the game to one CPU core; keeps the Jade engine's timing stable on modern many-core processors | **Keep this** |

The underlying reason GOG did it: the game holds an *exclusive fullscreen* Direct3D 9
device. Alt-tabbing away loses that device, and the game handles the loss badly — corrupt
HUD or a crash on return. Rather than fix the device-loss handling, the shipping fix
removed the feature that exposed it.

## The approach

A custom shim database is all-or-nothing — you cannot disable one shim inside it without
rebuilding the database. So this tool:

1. **Removes the shim database** using Microsoft's own supported tool, `sdbinst`.
   Not by renaming `BGE.exe` to defeat the filename match (see below).
2. **Restores single-core behaviour externally**, via a launcher shortcut that uses
   `start /affinity`. Child processes inherit processor affinity, so pinning the
   documented launch chain entry point covers the whole game:
   `CheckApplication.exe` → `run.exe` → `BGE.exe`.
3. **Takes the game out of exclusive fullscreen** by installing the bundled d3d9 proxy,
   so there is no device to lose in the first place. This addresses the *original* bug,
   not just the symptom — without it, Alt+Tab works but the HUD can still corrupt.

Every change is backed up and reversible.

### Why not just rename `BGE.exe`?

That is the widely-circulated fix, and it works by making the shim database's filename
match fail. It is strictly worse:

- It silently drops `SingleProcAffinity` too, with nothing put back in its place.
- It leaves a shim database registered against a file that no longer exists.
- GOG Galaxy's verify/repair renames the executable back, silently undoing it.

Same outcome, achieved by breaking a pattern match rather than removing the entry.

---

## Requirements

- Windows 10 or 11
- PowerShell 5.1 (ships with Windows) or newer
- Administrator rights — `sdbinst` modifies machine-scope compatibility settings
- The GOG release of *Beyond Good & Evil*

Developed and tested against the GOG build with `BGE.exe` at **7,778,304 bytes**. Other
builds are detected but reported as unrecognised.

## Usage

Check what state your install is in. Read-only, requires no elevation:

```bash
powershell -ExecutionPolicy Bypass -File .\Fix-BGEAltTab.ps1 -Status
```

Preview every change without touching anything. Also needs no elevation:

```bash
powershell -ExecutionPolicy Bypass -File .\Fix-BGEAltTab.ps1 -WhatIf
```

Apply the fix. Prompts for confirmation and elevates automatically:

```bash
powershell -ExecutionPolicy Bypass -File .\Fix-BGEAltTab.ps1
```

Undo everything:

```bash
powershell -ExecutionPolicy Bypass -File .\Fix-BGEAltTab.ps1 -Revert
```

### Parameters

| Parameter | Default | Description |
|---|---|---|
| `-GamePath <path>` | auto-detect | Install folder. Found via the GOG registry entry or common locations. |
| `-Status` | — | Report current state and exit. Changes nothing. |
| `-Revert` | — | Undo all changes. |
| `-AffinityMask <int>` | `1` | CPU bitmask for the launcher. `1` = first core (matches the shim). `3` = first two cores. |
| `-WindowMode <0-2>` | `1` | 0 = windowed, 1 = borderless centred, 2 = borderless stretched. |
| `-ProxyPath <path>` | `dist\d3d9.dll` | Prebuilt proxy to install. Verified 32-bit before use. |
| `-InstallControllerSupport` | — | Add XInput (Xbox) controller support. |
| `-PadLookSpeed <100-20000>` | `1800` | Right-stick look speed, in mouse counts per second at full deflection. Frame-rate independent. |
| `-PadDeadzone <0-32000>` | `7849` | Left-stick deadzone, raw XInput units. Default is XInput's recommended value. |
| `-PadLookDeadzone <0-32000>` | `8689` | Right-stick deadzone, raw XInput units. Default is XInput's recommended value. |
| `-PadInvertLook` | — | Invert the right stick's vertical axis. |
| `-PadPath <path>` | `dist\dinput8.dll` | Prebuilt controller proxy to install. |
| `-ResetConfig` | — | Regenerate the `.ini` files. Without it, existing configs are preserved. |
| `-NoShortcut` | — | Skip the launcher. **Leaves you with no affinity pinning at all.** |
| `-NoWindowedProxy` | — | Skip the proxy. **Leaves the game in exclusive fullscreen.** |
| `-NoElevate` | — | Error out instead of prompting for elevation. |
| `-Force` | — | Skip confirmation prompts. |

`-WhatIf` and `-Confirm` are supported throughout.

---

## The windowed proxy

[`src/d3d9_windowed.cpp`](src/d3d9_windowed.cpp) is a small Direct3D 9 proxy that does
exactly one thing: rewrite `D3DPRESENT_PARAMETERS::Windowed` to `TRUE`, zero the
fullscreen refresh rate (illegal once windowed), and restyle the game window. It does not
touch aspect ratio, resolution, FOV, or shaders.

It applies that edit at **both** places a D3D9 application sets the field:

| Call site | When the game reaches it |
| --- | --- |
| `IDirect3D9::CreateDevice` | once, at startup |
| `IDirect3DDevice9::Reset` | device-lost recovery, and any resolution or display change |

Intercepting only `CreateDevice` produces a fix that looks correct at launch and quietly
expires the first time the game resets — change the resolution in `SettingsApplication.exe`
or lose the device for any reason, and the game resets with `Windowed = FALSE`, drops back
into exclusive fullscreen, and the HUD corruption returns mid-session. So the device
returned by `CreateDevice` is wrapped too, and both paths run the same edit.
`CreateAdditionalSwapChain` is a third present-parameters entry point and is covered for
completeness, though D3D9 already requires windowed there.

It exists so this tool has no external dependency. The alternative was pulling a 1.29 MB
unverifiable binary from an anonymous uploader — behind a Cloudflare check, with no stable
download URL — in order to set one boolean.

### Design notes

- **Interfaces come from the Windows SDK.** `d3d9.h` ships under
  `Include\<ver>\shared` (not `um\`, which is where people tend to look and conclude it
  is missing); `dinput.h` and `xinput.h` are under `um\`. The proxies derive from
  `IDirect3D9`, `IDirectInput8A` and `IDirectInputDevice8A`, so the compiler emits the
  vtables and every slot and signature is checked at build time. Hand-declaring these
  layouts to skip an `#include` trades a compile error for silent stack corruption —
  it already cost this project one bug, in `IDirect3D9::GetAdapterMonitor`. The legacy
  DirectX SDK is still not required.
- **32-bit, always.** `BGE.exe` is a 32-bit process and silently fails to load a 64-bit
  DLL. The build forces `/MACHINE:X86` and the installer re-verifies the PE machine type
  before copying anything.
- **Chaining.** Only one file can be called `d3d9.dll`. If the game folder already has one
  (dgVoodoo, DXVK, ReShade), the installer renames it to `d3d9_chain.dll` and the proxy
  forwards to it; otherwise it falls back to `System32\d3d9.dll`. Ordering is
  game → proxy → chained DLL → system, so a chained DLL sees the modified parameters and
  could override them.
- **No `LoadLibrary` in `DllMain`.** The chain is resolved lazily on first export use;
  loading inside the loader lock would deadlock.
- **`CheckDeviceType` is answered for the windowed case**, because the game probes device
  support before `CreateDevice` and would otherwise bail on a combination it never gets.

### Building

Needs Visual Studio Build Tools with the C++ workload, which brings the Windows SDK that
supplies `d3d9.h`, `dinput.h`, `xinput.h` and `dxguid.lib`. The legacy DirectX SDK is not
needed.

```bash
src\build.cmd
```

Output is `dist\d3d9.dll` and `dist\dinput8.dll`. Prebuilt copies are committed, so this is
only needed if you change the source.

The functional tests drive both proxies the way `BGE.exe` does — the d3d9 test requests an
exclusive-fullscreen device and asserts the rewrite happened; the dinput8 test creates a
real DirectInput keyboard, acquires it, and reads state through the proxy. Both also call
methods far down the vtable (`GetAdapterMonitor`, `Poll`) so that layout drift between a
wrapper and the real interface shows up as a failing test rather than a crash in-game:

```bash
src\build_test.cmd
```

```
PASS proxy d3d9.dll loads          PASS proxy forced Windowed=TRUE
PASS Direct3DCreate9 exported      PASS refresh rate zeroed for windowed
PASS CreateDevice succeeded        PASS window restyled to WS_POPUP
PASS device is wrapped             PASS proxy forced Windowed=TRUE on Reset
PASS Reset succeeded               PASS window still borderless after Reset

PASS proxy dinput8.dll loads       PASS SetDataFormat accepted
PASS DirectInput8Create succeeded  PASS Acquire succeeded
PASS keyboard device created       PASS GetDeviceState succeeded
SKIP XInput pad connected on port 0 -> none detected, injection not exercised
```

A controller is not required to run the suite. When none is attached that last line reads
`SKIP` and the run still passes — everything above it exercises the proxy itself.

Pass a number of seconds to watch live controller-to-key translation, which is the quickest
way to check a mapping without launching the game:

```bash
src\build_test.cmd 15
```

### Configuration

`d3d9_windowed.ini`, written next to the DLL in the game folder:

```ini
[Display]
Mode=1     ; 0 = windowed, 1 = borderless centred, 2 = borderless stretched
Log=0      ; 1 writes d3d9_windowed.log next to the DLL
Chain=d3d9_chain.dll
```

Mode 1 preserves the game's aspect ratio, sizing the window to whatever resolution
`SettingsApplication.exe` is set to. Mode 2 fills the monitor but will distort 4:3 content
on a 16:9 display, since D3D stretches the backbuffer to the client area.

---

## Controller support (XInput)

```bash
powershell -ExecutionPolicy Bypass -File .\Fix-BGEAltTab.ps1 -InstallControllerSupport
```

The GOG release famously has no controller support, and that is not a setting anyone
turned off. `BGE.exe` imports exactly one symbol from `dinput8.dll` —
`DirectInput8Create` — and the binary contains no joystick, gamepad, axis or deadzone
vocabulary at all. Gamepad handling only ever existed in the later Steam/Uplay builds.

What the game *does* have is a DirectInput keyboard and mouse, and its bindings live in
the registry as DirectInput scan codes:

| Binding | Stored value | High word | Key |
|---|---|---|---|
| Up / Down / Left / Right | `0x00110008` … | `0x11 0x1F 0x1E 0x20` | `W S A D` |
| Run / Accelerate | `0x0039001D` | `0x39` | Space |
| Crouch | `0x001D001D` | `0x1D` | LCtrl |
| Use object / Buddy | `0x00100013` / `0x0012000F` | `0x10` / `0x12` | Q / E |
| Primary / Secondary action | `0x01020000` / `0x01030000` | mouse | LMB / RMB |

So the game reads a 256-byte DirectInput keyboard state array indexed by scan code. That
makes the reliable approach a `dinput8.dll` proxy that writes controller state directly
into the buffer the game reads — rather than synthesising OS-level input with `SendInput`
and hoping an exclusive-mode DirectInput device picks it up.

[`src/dinput8_xinput.cpp`](src/dinput8_xinput.cpp) wraps `IDirectInput8::CreateDevice`.
For the system keyboard and mouse it returns a device wrapper that merges XInput-derived
state into both `GetDeviceState` (immediate mode) and `GetDeviceData` (buffered mode);
every other device forwards untouched.

### Default mapping

| Control | Action | Sends |
|---|---|---|
| Left stick | Move | `W A S D` |
| Right stick | Look | relative mouse |
| A / B | Primary / secondary action | LMB / RMB |
| X / Y | Use object / buddy | `Q` / `E` |
| LB / RB | Photo mode / center view | `LShift` / `C` |
| LT / RT | Crouch / run · accelerate | `LCtrl` / `Space` |
| Start / Back | Menu / map | `Esc` / `Tab` |
| D-pad ←→ | Inventory prev / next | `2` / `3` |

Everything is remappable in `dinput8_xinput.ini` next to the game executable. Values are
DirectInput scan codes in hex, or `MOUSE1`..`MOUSE8`; `0` unmaps. `Log=1` writes
`dinput8_xinput.log` for troubleshooting.

Movement is digital because the game has no analog movement path — there is nothing to
feed an analog value into. Look is genuinely analog, since it becomes relative mouse
motion.

### Input model

The keyboard and mouse wrappers are two objects that have to agree about one controller,
and look has to mean the same thing at any frame rate. Three rules do that:

- **One sample, shared.** The pad is read into a single immutable snapshot under a lock
  and copied out by value. A snapshot is reused for 2 ms, so both devices polled in the
  same frame see the *same instant* rather than each re-reading XInput. Everything else —
  edge-detection buffers, the look carry — is per device, so there is nothing left to
  race on.
- **Look is a velocity, not a per-poll delta.** The right stick is stored as −1..1, and
  the mouse wrapper multiplies by `LookSpeed` and by the time actually elapsed since it
  last reported motion (`QueryPerformanceCounter`). `LookSpeed` is therefore mouse counts
  per second — a real unit. Adding a fixed delta per poll instead makes speed scale with
  the poll rate: the same setting turned **2.4× faster at 144 Hz than at 60 Hz**. The
  fractional remainder carries between calls, so a stick held just off centre moves the
  camera slowly instead of truncating to zero.
- **Finding the pad is throttled.** `XInputGetState` on an empty slot is a documented
  slow path that Microsoft says not to call every frame. Walking indices 0–3 until one
  answered did exactly that, from both wrapped devices — about **1150 slow calls a
  second** at 144 Hz on a machine with no controller, which is most machines running an
  Alt+Tab fix, in a game pinned to one core for timing stability. The connected index is
  now remembered, so a poll costs one call; all four slots are only swept when nothing is
  connected *or* the current pad is idle, and then at most every two seconds. Scanning
  while idle is what lets you put down pad 0 and pick up pad 1 — previously the loop
  stopped at the first slot that answered, so a connected-but-untouched pad 0 meant pad 1
  was never read at all.
- **The deadzone is radial and rescaled.** Thresholding each axis separately leaves a
  cross-shaped dead region — at `Deadzone=8000` a diagonal push of `(8000, 8000)` failed
  both axis tests despite being 11313 units out, so diagonals needed a harder push than
  cardinals. It also stepped: output jumped from 0 straight to the raw axis value on
  crossing. Now the test is on vector length and `[deadzone, full]` is remapped onto
  `[0, 1]`, so the dead region is a disc and the response ramps from zero.
- **Injected events are numbered in DirectInput's namespace.** In buffered mode the
  synthetic events go into the same buffer as DirectInput's own, and applications compare
  `dwSequence` with `DISEQUENCE_COMPARE` to order events across devices. A private counter
  starting at 1 — the previous behaviour — is not in that namespace at all: every
  injected event sorted before every real one, forever. There is no API to read
  DirectInput's counter, so the proxy anchors instead, tracking the highest real sequence
  number seen on either wrapped device and issuing synthetic events just above it.

The maths lives in [`src/pad_support.h`](src/pad_support.h), which the proxy and the test
suite both compile, so it is asserted on numerically with no controller attached:

```
PASS diagonal at the per-axis threshold is live -> len=130/1000 at raw(7849,7849)
PASS dead region is circular, not cross-shaped  -> 0deg=487 45deg=487 90deg=487 (/1000)
PASS response ramps from the deadzone edge      -> out=0/1000 just past edge
PASS look speed is frame-rate independent       -> 30Hz=1800 60Hz=1800 144Hz=1800 counts/sec
PASS sub-count motion is carried, not truncated -> 36 counts/sec at 2% deflection
PASS unplugged cost is bounded                  -> 4 calls/sec unplugged at 144Hz, was 1152
PASS no scans at all while playing              -> 0 scans in a second of play
PASS synthetic event sorts after a delivered real event -> real=500000 synthetic=500001
PASS re-anchors to a newer real sequence        -> last=500100 real=900000 -> 900001
```

### Scope

The exported entry points are complete and every non-injected call forwards untouched, so
another `dinput8` client that loads this DLL keeps working. The **injection**, though, is
tuned for BGE, and one limit is worth stating rather than discovering:

Sequence anchoring is correct against every event already delivered, but it cannot see
into the future. A long burst of injected events with no real input in between runs our
numbers ahead, so a real event arriving afterwards can land below one of ours. The window
is bounded by the gap between real events — and inside that gap there is, by definition,
no real event to be mis-ordered against. Still, an application that depends on exact
cross-device ordering under `DISEQUENCE_COMPARE` should not assume this DLL is
transparent. It is a BGE shim that behaves itself toward other clients, not a
general-purpose `dinput8` replacement.

**Upgrading:** `LookSensitivity` was counts per poll and has been replaced by `LookSpeed`
in counts per second. An older `.ini` still carrying `LookSensitivity` is not converted —
the key is ignored and `LookSpeed` falls back to its default. Run with `-ResetConfig` to
regenerate the file, or add a `LookSpeed` line yourself.

---

## What it changes

| Change | Location |
|---|---|
| Shim database uninstalled | `HKLM\...\AppCompatFlags\Custom\BGE.exe` + `%WINDIR%\AppPatch\CustomSDB\` |
| Launcher shortcut created | `%USERPROFILE%\Desktop\Beyond Good & Evil (Alt+Tab Fix).lnk` |
| Proxy + config installed | `<game>\d3d9.dll`, `<game>\d3d9_windowed.ini` |
| Controller proxy + config | `<game>\dinput8.dll`, `<game>\dinput8_xinput.ini` — only with `-InstallControllerSupport` |
| Pre-existing DLLs renamed | `<game>\d3d9_chain.dll`, `<game>\dinput8_chain.dll` (restored on `-Revert`) |
| Backups + revert data | `%ProgramData%\BGEAltTabFix\` |

The original `goggame.sdb` also remains untouched in the game folder, so the change is
reversible even if the backup directory is deleted.

## Important limitations

**Launch the game from the new shortcut.** Removing the shim database is machine-wide, so
Alt+Tab is fixed no matter how you start the game — but processor affinity is only applied
by the shortcut. Starting the game from GOG Galaxy, the Start Menu, or the original desktop
icon gives you Alt+Tab *without* single-core pinning. If you see timing or audio problems,
that is why. (To fix this at the source, point GOG Galaxy's launch task at the new shortcut,
or use the ADK route below.)

**Reinstalling the game restores the shim.** GOG's installer gates the shim install behind
a `DoSDBOnce1207658746=1` flag in `goglog.ini`. A fresh install or a repair can re-register
it. Just re-run this script — see below.

## Idempotency

Re-running is safe and is the supported way to repair or upgrade an install. Specifically:

- Removing the shim is skipped when it is already gone.
- The launcher shortcut is rewritten in place.
- A proxy already installed is replaced by the current build. Our DLLs carry an embedded
  marker (`BGEFIX_PROXY_V1`), so an upgraded build recognises an older build of itself
  rather than mistaking it for a third-party wrapper and chaining to it.
- A third-party DLL that was chained on a previous run stays chained, and is re-recorded
  each run so `-Revert` can always put it back.
- **Your `.ini` files are never overwritten.** Once a config exists it is left alone, so
  remapping survives every re-run. Use `-ResetConfig` to regenerate from the defaults.

The one state that is deliberately *not* auto-resolved: if both `d3d9.dll` and
`d3d9_chain.dll` (or the `dinput8` pair) exist and the first is not ours, the installer
refuses rather than guess which to keep.

**Only databases containing `IgnoreAltTab` are removed.** If a database is registered for
`BGE.exe` that the script cannot identify, it refuses to touch it rather than guessing.

## The purist alternative

The fully surgical fix for step 2 is to rebuild the shim database containing *only*
`SingleProcAffinity`, using **Compatibility Administrator** from the
[Windows ADK](https://learn.microsoft.com/en-us/windows-hardware/get-started/adk-install).
That keeps affinity applied by Windows itself regardless of how the game is launched, with
no launcher shortcut needed. It reaches the same end state as this script, minus the
launcher caveat, at the cost of an ADK download. This script exists because most people
will not install the ADK to play a 2003 adventure game.

## Credits

Root cause and shim inventory determined by inspecting the shipped `goggame.sdb` and the
registered compatibility database. Corroborating documentation:

- [PCGamingWiki — Beyond Good & Evil](https://www.pcgamingwiki.com/wiki/Beyond_Good_%26_Evil)

Not affiliated with GOG.com or Ubisoft. *Beyond Good & Evil* is a trademark of Ubisoft.

## License

MIT — see [LICENSE](LICENSE).
