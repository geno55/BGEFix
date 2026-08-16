# BGEFix

Fixes for the GOG.com release of *Beyond Good & Evil* (2003), with no third-party
downloads. Three independent parts, installable separately:

- **Alt+Tab** — restored, without throwing away the compatibility shim you actually want.
- **Windowed mode** — ends the exclusive-fullscreen device that caused the problem GOG
  papered over.
- **Controller support** — XInput gamepads, which the GOG build has no code for at all.

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

In the build tested here it applies exactly two shims to `BGE.exe`:

| Shim | Effect | Verdict |
|---|---|---|
| `IgnoreAltTab` | Makes the game swallow Alt+Tab entirely | **This is the bug** |
| `SingleProcAffinity` | Pins the game to one CPU core; keeps the Jade engine's timing stable on modern many-core processors | **Keep this** |

That table is what *this* database contained when it was examined, not an assumption the
tool makes. `-Status` enumerates the database on your machine by parsing it, so if GOG
reissues it with a third shim you will be told — see
[Reading the shim database](#reading-the-shim-database).

The underlying reason GOG did it: the game holds an *exclusive fullscreen* Direct3D 9
device. Alt-tabbing away loses that device, and the game handles the loss badly — corrupt
HUD or a crash on return. Rather than fix the device-loss handling, the shipping fix
removed the feature that exposed it.

## The approach

A custom shim database is all-or-nothing — you cannot disable one shim inside it without
rebuilding the database. So this tool rebuilds it:

1. **Removes GOG's shim database** using Microsoft's own supported tool, `sdbinst`.
   Not by renaming `BGE.exe` to defeat the filename match (see below).
2. **Installs a replacement database that applies only `SingleProcAffinity`**, derived
   from GOG's own by deleting the `IgnoreAltTab` entry. Affinity stays a property of
   `BGE.exe` enforced by Windows, so it applies however the game is started — GOG Galaxy,
   a Steam shortcut, or the executable directly.
3. **Takes the game out of exclusive fullscreen** by installing the bundled d3d9 proxy,
   so there is no device to lose in the first place. This addresses the *original* bug,
   not just the symptom — without it, Alt+Tab works but the HUD can still corrupt.

Nothing is added to your desktop. Both fixes are properties of `BGE.exe` applied by Windows
itself, so it does not matter how you start the game. If the replacement database cannot be
built or does not register, the tool says so loudly rather than quietly handing you a
shortcut you would have to remember to use.

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
powershell -ExecutionPolicy Bypass -File .\Fix-BGE.ps1 -Status
```

Preview every change without touching anything. Also needs no elevation:

```bash
powershell -ExecutionPolicy Bypass -File .\Fix-BGE.ps1 -WhatIf
```

Apply the fix. Prompts for confirmation and elevates automatically:

```bash
powershell -ExecutionPolicy Bypass -File .\Fix-BGE.ps1
```

Undo everything:

```bash
powershell -ExecutionPolicy Bypass -File .\Fix-BGE.ps1 -Revert
```

### Parameters

| Parameter | Default | Description |
|---|---|---|
| `-GamePath <path>` | auto-detect | Install folder. Found via the GOG registry entry or common locations. |
| `-Status` | — | Report current state and exit. Changes nothing. |
| `-Revert` | — | Undo all changes. |
| `-WindowMode <0-2>` | `1` | 0 = windowed, 1 = borderless centred, 2 = borderless stretched. |
| `-ProxyPath <path>` | `dist\d3d9.dll` | Prebuilt proxy to install. Verified 32-bit before use. |
| `-Component <list>` | `AltTab,Windowed` | Which parts to act on — see below. `All` selects everything. Also applies to `-Revert`, where it defaults to `All`. |
| `-PadLookSpeed <100-20000>` | `1800` | Right-stick look speed, in mouse counts per second at full deflection. Frame-rate independent. |
| `-PadDeadzone <0-32000>` | `7849` | Left-stick deadzone, raw XInput units. Default is XInput's recommended value. |
| `-PadLookDeadzone <0-32000>` | `8689` | Right-stick deadzone, raw XInput units. Default is XInput's recommended value. |
| `-PadInvertLook` | — | Invert the right stick's vertical axis. |
| `-PadPath <path>` | `dist\dinput8.dll` | Prebuilt controller proxy to install. |
| `-ResetConfig` | — | Regenerate the `.ini` files. Without it, existing configs are preserved. |
| `-NoElevate` | — | Error out instead of prompting for elevation. Never forwarded to the elevated copy. |
| `-Force` | — | Skip confirmation prompts. |

`-WhatIf` and `-Confirm` are supported throughout.

### Components

This tool does three separable things, and the controller proxy in particular has nothing
to do with Alt+Tab. Each is selected by name, with one polarity:

| Component | What it does | Needs the others? |
|---|---|---|
| `AltTab` | Replaces GOG's shim database with one applying only `SingleProcAffinity` | no |
| `Windowed` | Installs the d3d9 proxy, ending exclusive fullscreen | no |
| `Controller` | Installs the dinput8 XInput proxy | **no** |

```bash
powershell -ExecutionPolicy Bypass -File .\Fix-BGE.ps1 -Component Controller
```

That installs gamepad support and touches nothing else — no compatibility database is
removed, no shortcut appears. Previously the only way to get controller support was
`-InstallControllerSupport`, which ran the entire apply path.

`-Revert` takes the same list, so `-Revert -Component Controller` removes the gamepad
proxy and leaves everything else installed.

Selection used to be `-NoShortcut`, `-NoWindowedProxy` and `-InstallControllerSupport` —
two opt-outs and an opt-in for three co-equal features. **They have been removed**, not
deprecated: passing one is now a parameter binding error. That is loud and changes
nothing, where a silent remap of a misread flag would not be.

| Was | Now |
|---|---|
| *(default)* | *(unchanged, minus the desktop shortcut)* |
| `-NoShortcut` | *(now the only behaviour — the launcher is gone)* |
| `-NoWindowedProxy` | `-Component AltTab` |
| `-InstallControllerSupport` | `-Component All` |
| `-AffinityMask <n>` | *(removed with the launcher; the shim pins one core)* |
| — | `-Component Controller` (new: gamepad only) |

**Contradictions are now binding errors**, not silently-resolved precedence. `-Status`,
`-Revert` and the install path are separate parameter sets, and apply-only tuning is not
declared in the others:

```
-Status -Revert            -> Parameter set cannot be resolved
-Revert -WindowMode 2      -> Parameter set cannot be resolved
-Component Controller -WindowMode 2
                           -> -WindowMode only applies to the Windowed component
```

The last one is a runtime check: parameter sets cannot express "only meaningful when
`-Component` includes `Windowed`", and silently ignoring the setting is how people come to
believe they configured something they did not.

### Running it unattended

`-Force` skips the confirmation prompt, and the script is safe to drive from another
script:

- **The exit code is real.** When the script elevates itself it waits for the elevated
  copy and returns *its* exit code. 0 means the work succeeded; non-zero means it failed
  or you cancelled the UAC prompt. Earlier versions launched the elevated copy and
  returned immediately, so the caller always saw success.
- **`-Force` actually runs unattended.** The elevated window closes on its own. Without
  `-Force` it pauses on a keypress instead, so you can read the summary.
- **Nothing is written to a user profile**, so it does not matter that UAC elevates into a
  different account on a standard user. Everything the tool installs is machine-scope or
  lives in the game folder, and `-Status` run unelevated afterwards sees exactly what was
  installed.

```bash
powershell -ExecutionPolicy Bypass -File .\Fix-BGE.ps1 -Force
```

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
- **One copy of the plumbing.** The two DLLs intercept different APIs but are the same
  kind of thing: a file in the game folder wearing a system DLL's name, reading an ini
  and writing a log beside itself, carrying the installer marker, and forwarding what it
  does not intercept. That code lives once in
  [`src/proxy_common.h`](src/proxy_common.h); each proxy states only what its files are
  called — ini base name, chain filename, and the system DLL to fall back to. The shared
  keys are read from `[General]` in both. It was previously pasted into both `.cpp`
  files, where the copies had already begun to drift — including reading `Log` and
  `Chain` from a different ini section in each.
- **A degraded proxy says so.** If a wrapper cannot be allocated, or no XInput runtime is
  present, the DLL hands the real interface through so the game keeps running — but that
  is a behaviour change the player would otherwise have no way to see, and for the d3d9
  proxy it means silently returning to the exclusive fullscreen the fix exists to
  prevent. Every such site reports through `ProxyDegraded`, which writes to the log *even
  with `Log=0`* (the default — a warning only visible to someone who already enabled
  logging reaches nobody) and shows one message box per process naming what was lost.
  `BGEFIX_NO_UI=1` in the environment suppresses the box; the log line still goes out.
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

`build_test.cmd` also builds a second, throwaway pair of DLLs with `BGEFIX_TEST_OOM`
defined, in which wrapper allocations fail on demand, and runs the tests against them:

```
PASS the game still gets a usable IDirect3D9   PASS this session is still windowed
PASS the game still gets a working device      PASS the failure is logged even with Log=0
```

Those branches decide what the game does when the fix cannot be applied, and no ordinary
run ever reaches them, so they are walked deliberately rather than reasoned about.

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

[General]
Log=0      ; 1 writes d3d9_windowed.log next to the DLL
Chain=d3d9_chain.dll
```

`[General]` means the same thing in both proxies' ini files: `Log` and `Chain` are read
from it by `d3d9.dll` and `dinput8.dll` alike, so knowing one file tells you where to look
in the other. Settings specific to a proxy live in a section named for what they do —
`[Display]` here, `[Buttons]` and `[Sticks]` for the controller.

Mode 1 preserves the game's aspect ratio, sizing the window to whatever resolution
`SettingsApplication.exe` is set to. Mode 2 fills the monitor but will distort 4:3 content
on a 16:9 display, since D3D stretches the backbuffer to the client area.

---

## Controller support (XInput)

```bash
powershell -ExecutionPolicy Bypass -File .\Fix-BGE.ps1 -InstallControllerSupport
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
`dinput8_xinput.log` for troubleshooting — and if controller support cannot start at all
(no XInput runtime on the machine, or a failed allocation), that is written to the log and
shown in a dialog whether or not `Log` is on, rather than the pad quietly doing nothing.

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
| Shim database replaced | `HKLM\...\AppCompatFlags\Custom\BGE.exe` + `%WINDIR%\AppPatch\CustomSDB\` |
| Proxy + config installed | `<game>\d3d9.dll`, `<game>\d3d9_windowed.ini` — only with the `Windowed` component |
| Controller proxy + config | `<game>\dinput8.dll`, `<game>\dinput8_xinput.ini` — only with the `Controller` component |
| Pre-existing DLLs renamed | `<game>\d3d9_chain.dll`, `<game>\dinput8_chain.dll` (restored on `-Revert`) |
| Backups + revert data | `%ProgramData%\BGEFix\` |

The original `goggame.sdb` also remains untouched in the game folder, so the change is
reversible even if the backup directory is deleted.

### Upgrading from an older version

The state directory was renamed and the desktop launcher removed, and **nothing is
migrated**. If you applied the fix with an older build, revert with *that* build first.
Otherwise the old `%ProgramData%\BGEAltTabFix\` and any
`Beyond Good & Evil (Alt+Tab Fix).lnk` are stranded: this version does not know about them,
`-Revert` will not touch them, and deleting them is a manual job.

## Important limitations

**Start the game however you like.** Both fixes are machine-wide: Alt+Tab is fixed because
GOG's database is gone, and single-core pinning still applies because the replacement
database applies `SingleProcAffinity` to `BGE.exe` itself. GOG Galaxy, the Start Menu and
the original desktop icon all behave the same, and there is no special launcher to
remember.

If the replacement database could not be installed, the tool says so loudly and the game
is left unpinned — it does not silently substitute something weaker. `-Status` always
reports which state you are in.

**Reinstalling the game restores the shim.** GOG's installer gates the shim install behind
a `DoSDBOnce1207658746=1` flag in `goglog.ini`. A fresh install or a repair can re-register
it. Just re-run this script — see below.

## Idempotency

Re-running is safe and is the supported way to repair or upgrade an install. Specifically:

- Removing the shim is skipped when it is already gone.
- The replacement shim database is reinstalled over itself; its GUID is fixed, so
  re-running replaces rather than accumulating registrations.
- A proxy already installed is replaced by the current build. Our DLLs carry an embedded
  marker, so an upgraded build recognises an older build of itself rather than mistaking
  it for a third-party wrapper and chaining to it. See below.
- A third-party DLL that was chained on a previous run stays chained, and is re-recorded
  each run so `-Revert` can always put it back.
- **Your `.ini` files are never overwritten.** Once a config exists it is left alone, so
  remapping survives every re-run. Use `-ResetConfig` to regenerate from the defaults.

The one state that is deliberately *not* auto-resolved: if both `d3d9.dll` and
`d3d9_chain.dll` (or the `dinput8` pair) exist and the first is not ours, the installer
refuses rather than guess which to keep.

### Recognising our own DLLs

The marker is `BGEFIX_PROXY{502eb6b9-…}v2`. The GUID is the identity and never changes;
the trailing number is a version, and the installer **matches the prefix and parses the
number** rather than comparing the whole string.

That distinction is the entire point. An installer that looked for the exact marker it
ships would meet the *previous* release's DLL, fail to recognise it, classify it as a
third-party wrapper, rename it to `d3d9_chain.dll` and chain new-to-old — which is
precisely the failure the marker was introduced to prevent, arriving one release later on
schedule. Older markers are listed and still recognised; newer ones parse fine and are
replaced with a warning rather than chained to.

This answer is also the authority for overwriting a file in the game folder **without
backing it up**, so it is more than a substring search. The file must be a 32-bit PE, and
the version digits must be followed by a NUL, since the marker is a C string literal. That
rejects logs, configs, saves, 64-bit DLLs and chance occurrences inside unrelated blobs.
It remains evidence rather than proof — a string in a binary always is — but it fails
closed: anything unrecognised is treated as third-party, which means backed up and
chained, never silently replaced.

[`src/test_installer.ps1`](src/test_installer.ps1) covers this, and runs as part of
`src\build_test.cmd`. It synthesises markers this build does not carry — one older, one
newer — because the interesting failure only exists across releases and cannot be
reproduced with the shipping marker alone:

```
PASS a PREVIOUS release is still recognised as ours -> v1
PASS a FUTURE release is still recognised as ours   -> v99
PASS a text file containing the marker is rejected  -> not a 32-bit PE
PASS a PE whose marker is not NUL-terminated is rejected -> fails the C-string check
```

### Reading the shim database

Removing a machine-scope compatibility database is only defensible if you know what is in
it, so the tool reads it — it walks the `.sdb` tag tree through `apphelp.dll`, the same
parser Windows itself uses, and reports every executable the database patches and every
modification it applies to each.

It did not always. The previous implementation searched the raw bytes for the two shim
names the script already hardcodes, which made it structurally incapable of returning a
third: if GOG reissued the database with an extra shim, the tool would report the same two,
remove the database, and drop the third silently — while `-Status` printed the list as
though it were an inventory of the file.

Consequences of reading it for real:

- `-Status` lists what the database actually contains, and marks anything this tool does
  not account for as `<- NOT accounted for by this tool`.
- Applying the fix warns before the confirmation prompt if the database applies anything
  beyond the two known shims, naming what removal would drop.
- **A database that cannot be parsed is treated as unknown, not as empty**, and the tool
  refuses to remove it. "No shims found" and "could not read the file" are different
  answers and are no longer conflated.
- **Only databases that actually apply `IgnoreAltTab` are removed.** If a database is
  registered for `BGE.exe` that the script cannot identify, it refuses to touch it rather
  than guessing, and now says what it found instead.

[`src/test_installer.ps1`](src/test_installer.ps1) builds real `.sdb` files carrying shims
the tool has never heard of, which is the case the old implementation could not even
represent:

```
PASS an UNHARDCODED third shim is discovered -> shims: IgnoreAltTab, SingleProcAffinity, SomeFutureShim
PASS an unrelated database reports its real contents -> shims: WinXPSP3, Win8RTMVersionLie
PASS an unparseable database reports UNKNOWN, not empty -> Ok=False
PASS the real GOG database parses -> GOG.com Beyond Good and Evil: SingleProcAffinity, IgnoreAltTab
```

### Why derive it instead of shipping one

This repository commits prebuilt `dist\d3d9.dll` and `dist\dinput8.dll`, so shipping a
prebuilt `.sdb` would be consistent — and simpler. It is deliberately not done, for one
reason: the parts of that database that make the shim actually apply are GOG's, not ours.
The matching rules, the app name and vendor, the per-executable GUID and the name index
keyed on them are all their authored data. The DLLs are built from source in this repo and
are ours to ship; a GOG-authored compatibility database is not.

Deriving from the copy already on the user's disk avoids redistributing it, keeps every one
of those fields byte-identical so the shim still matches, and keeps working if GOG reissues
the database. It also means the tool preserves any *other* shim the database applies —
only `IgnoreAltTab` is removed, rather than a hardcoded set being reimposed.

## The launcher shortcut, and why it is gone

Earlier versions replaced `SingleProcAffinity` with a desktop shortcut using
`start /affinity`, and described rebuilding the database as a "purist alternative" needing
the [Windows ADK](https://learn.microsoft.com/en-us/windows-hardware/get-started/adk-install)'s
Compatibility Administrator.

Both halves of that were wrong. The shortcut was a downgrade dressed up as a replacement:
a shim the OS applied unconditionally became something you had to remember to click, and
`-NoShortcut` silently meant "no affinity at all". And the ADK was never required —
Compatibility Administrator *authors* an `.sdb`, it is not needed to *edit* one.

Once the replacement database landed, the shortcut had nothing left to do. It survived
briefly as a "convenience" that duplicated GOG's own desktop icon, and as a fallback for a
failed database install — but falling back to it would have meant reporting success while
delivering something weaker than what GOG shipped. It is now removed entirely, along with
`-AffinityMask`, `New-AffinityShortcut`, and the machinery for resolving the invoking
user's desktop across a UAC elevation. That last item was a genuine source of bugs and
existed solely to place this one file.

## Credits

Root cause and shim inventory determined by inspecting the shipped `goggame.sdb` and the
registered compatibility database. Corroborating documentation:

- [PCGamingWiki — Beyond Good & Evil](https://www.pcgamingwiki.com/wiki/Beyond_Good_%26_Evil)

Not affiliated with GOG.com or Ubisoft. *Beyond Good & Evil* is a trademark of Ubisoft.

## License

MIT — see [LICENSE](LICENSE).
