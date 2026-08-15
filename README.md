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

Output is `dist\d3d9.dll`. A prebuilt copy is committed, so this is only needed if you
change the source.

To run the functional test — it loads the DLL, requests an exclusive-fullscreen device the
way BGE does, and asserts the rewrite happened:

```bash
src\build_test.cmd
```

```
PASS proxy d3d9.dll loads          PASS proxy forced Windowed=TRUE
PASS Direct3DCreate9 exported      PASS refresh rate zeroed for windowed
PASS DebugSetMute exported         PASS window restyled to WS_POPUP
PASS CreateDevice succeeded        PASS client area matches backbuffer
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

## What it changes

| Change | Location |
|---|---|
| Shim database uninstalled | `HKLM\...\AppCompatFlags\Custom\BGE.exe` + `%WINDIR%\AppPatch\CustomSDB\` |
| Launcher shortcut created | `%USERPROFILE%\Desktop\Beyond Good & Evil (Alt+Tab Fix).lnk` |
| Proxy + config installed | `<game>\d3d9.dll`, `<game>\d3d9_windowed.ini` |
| Pre-existing `d3d9.dll` renamed | `<game>\d3d9_chain.dll` (restored on `-Revert`) |
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
it. Just re-run this script; it is idempotent.

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
