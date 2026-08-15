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
| `-PadLookSensitivity <1-200>` | `30` | Right-stick look speed. |
| `-PadDeadzone <0-32000>` | `8000` | Stick deadzone in raw XInput units. |
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
exactly one thing: rewrite `D3DPRESENT_PARAMETERS::Windowed` to `TRUE` in
`IDirect3D9::CreateDevice`, zero the fullscreen refresh rate (illegal once windowed), and
restyle the game window. It does not touch aspect ratio, resolution, FOV, or shaders.

It exists so this tool has no external dependency. The alternative was pulling a 1.29 MB
unverifiable binary from an anonymous uploader — behind a Cloudflare check, with no stable
download URL — in order to set one boolean.

### Design notes

- **No DirectX headers.** `d3d9.h` is not in a default Windows SDK install. Since the
  proxy only ever forwards vtable slots and never calls a D3D method by name, it declares
  the `IDirect3D9` vtable layout and `D3DPRESENT_PARAMETERS` directly. Only `windows.h`,
  `kernel32` and `user32` are needed.
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

Needs Visual Studio Build Tools with the C++ workload. No DirectX SDK.

```bash
src\build.cmd
```

Output is `dist\d3d9.dll` and `dist\dinput8.dll`. Prebuilt copies are committed, so this is
only needed if you change the source.

The functional tests drive both proxies the way `BGE.exe` does — the d3d9 test requests an
exclusive-fullscreen device and asserts the rewrite happened; the dinput8 test creates a
real DirectInput keyboard, acquires it, and reads state through the proxy:

```bash
src\build_test.cmd
```

```
PASS proxy d3d9.dll loads          PASS proxy forced Windowed=TRUE
PASS Direct3DCreate9 exported      PASS refresh rate zeroed for windowed
PASS CreateDevice succeeded        PASS window restyled to WS_POPUP

PASS proxy dinput8.dll loads       PASS SetDataFormat accepted
PASS DirectInput8Create succeeded  PASS Acquire succeeded
PASS keyboard device created       PASS GetDeviceState succeeded
```

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
| LB / RB | Look mode / center view | `LShift` / `C` |
| LT / RT | Crouch / run · accelerate | `LCtrl` / `Space` |
| Start / Back | Menu / map | `Esc` / `Tab` |
| D-pad ←→ | Inventory prev / next | `2` / `3` |

Everything is remappable in `dinput8_xinput.ini` next to the game executable. Values are
DirectInput scan codes in hex, or `MOUSE1`..`MOUSE8`; `0` unmaps. `Log=1` writes
`dinput8_xinput.log` for troubleshooting.

Movement is digital because the game has no analog movement path — there is nothing to
feed an analog value into. Look is genuinely analog, since it becomes relative mouse
motion.

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
