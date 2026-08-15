<#
.SYNOPSIS
    Re-enables Alt+Tab in the GOG.com release of Beyond Good & Evil (2003).

.DESCRIPTION
    The GOG release installs a custom Windows Application Compatibility shim
    database (shipped as 'goggame.sdb' in the game folder). It applies two shims
    to BGE.exe:

        IgnoreAltTab        - makes the game swallow Alt+Tab entirely.
        SingleProcAffinity  - pins the game to a single CPU core, which keeps the
                              Jade engine's timing stable on modern processors.

    The first is why you cannot Alt+Tab. The second is a fix you actually want.
    Because a custom shim database is all-or-nothing, this script removes the
    database with Microsoft's supported tool (sdbinst) and then restores the
    single-core behaviour externally via a launcher shortcut that sets processor
    affinity. Child processes inherit affinity, so pinning the documented launch
    chain entry point (CheckApplication.exe -> run.exe -> BGE.exe) covers the game.

    It also installs the bundled d3d9 proxy (dist\d3d9.dll), which forces the game
    out of exclusive fullscreen. Without that the game holds an exclusive Direct3D 9
    device, and alt-tabbing away can corrupt the HUD or crash on return - which is
    the bug GOG papered over by disabling Alt+Tab in the first place.

    Every change is backed up and reversible with -Revert.

.PARAMETER GamePath
    Path to the Beyond Good & Evil install folder. Auto-detected from the GOG
    registry entry or common install locations when omitted.

.PARAMETER Revert
    Undo everything: reinstall the original shim database, remove the launcher
    shortcut, and remove the proxy.

.PARAMETER Status
    Report the current state and exit without changing anything.

.PARAMETER AffinityMask
    CPU affinity bitmask for the launcher shortcut. Default 1 (first core only),
    matching what the SingleProcAffinity shim does. Use 3 for the first two cores.

.PARAMETER NoShortcut
    Do not create the affinity launcher shortcut.

.PARAMETER NoWindowedProxy
    Do not install the d3d9 proxy. Alt+Tab still works, but the game keeps an
    exclusive fullscreen device, so the HUD can corrupt when you return to it.

.PARAMETER ProxyPath
    Path to a prebuilt proxy DLL. Defaults to dist\d3d9.dll beside this script.
    Must be 32-bit; this is verified before installing.

.PARAMETER WindowMode
    0 = windowed with a title bar, 1 = borderless centred at the game's resolution
    (default), 2 = borderless stretched to fill the monitor.

.PARAMETER NoElevate
    Fail with an error instead of relaunching elevated when not running as admin.

.PARAMETER Force
    Skip the interactive confirmation prompt.

.EXAMPLE
    .\Fix-BGEAltTab.ps1 -Status

.EXAMPLE
    .\Fix-BGEAltTab.ps1

.EXAMPLE
    .\Fix-BGEAltTab.ps1 -GamePath "D:\Games\Beyond Good and Evil" -Force

.EXAMPLE
    .\Fix-BGEAltTab.ps1 -WindowMode 2 -Force

.EXAMPLE
    .\Fix-BGEAltTab.ps1 -Revert

.NOTES
    Requires administrator rights (sdbinst modifies machine-scope compatibility
    settings). Tested against the GOG build with BGE.exe at 7,778,304 bytes.

    License: MIT. Not affiliated with GOG.com or Ubisoft.
#>
# Note: #Requires must stay below the help block. Placed above it, PowerShell stops
# recognising the block as comment-based help and Get-Help falls back to bare syntax.
#Requires -Version 5.1
[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Medium')]
param(
    [string]   $GamePath,
    [switch]   $Revert,
    [switch]   $Status,
    [int]      $AffinityMask = 1,
    [switch]   $NoShortcut,
    [switch]   $NoWindowedProxy,
    [string]   $ProxyPath,
    [ValidateRange(0, 2)]
    [int]      $WindowMode = 1,
    [switch]   $NoElevate,
    [switch]   $Force
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

#region ---------------------------------------------------------------- constants

$script:AppName      = 'BGE Alt+Tab Fix'
$script:StateDir     = Join-Path $env:ProgramData 'BGEAltTabFix'
$script:BackupDir    = Join-Path $script:StateDir  'backup'
$script:StateFile    = Join-Path $script:StateDir  'state.json'
$script:ShortcutName = 'Beyond Good & Evil (Alt+Tab Fix).lnk'

# The shim we are targeting, and the one we must preserve the effect of.
$script:BadShim      = 'IgnoreAltTab'
$script:GoodShim     = 'SingleProcAffinity'

# GOG product id for Beyond Good & Evil.
$script:GogGameId    = '1207658746'

$script:CustomKey    = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Custom'
$script:InstalledKey = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\InstalledSDB'

# Documented launch chain. Affinity set on the first present entry is inherited
# by everything it spawns.
$script:LaunchChain  = @('CheckApplication.exe', 'run.exe', 'BGE.exe')

#endregion

#region ---------------------------------------------------------------- output helpers

function Write-Head {
    param([string]$Text)
    Write-Host ''
    Write-Host "  $Text" -ForegroundColor Cyan
    Write-Host "  $('-' * $Text.Length)" -ForegroundColor DarkCyan
}

function Write-Step { param([string]$Text) Write-Host "  [>] $Text" -ForegroundColor Gray }
function Write-Ok   { param([string]$Text) Write-Host "  [+] $Text" -ForegroundColor Green }
function Write-Warn2{ param([string]$Text) Write-Host "  [!] $Text" -ForegroundColor Yellow }
function Write-Bad  { param([string]$Text) Write-Host "  [x] $Text" -ForegroundColor Red }
function Write-Info { param([string]$Text) Write-Host "      $Text" -ForegroundColor DarkGray }

#endregion

#region ---------------------------------------------------------------- environment

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $pr = New-Object Security.Principal.WindowsPrincipal($id)
    return $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Invoke-SelfElevate {
    # Rebuild the original invocation so the elevated copy does the same thing.
    # -NoExit keeps the elevated console open; without it the window closes the moment
    # the script finishes and the summary is never readable.
    $argList = @('-NoExit', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"")

    foreach ($kv in $PSBoundParameters.GetEnumerator()) {
        if ($kv.Key -eq 'NoElevate') { continue }
        if ($kv.Value -is [switch]) {
            if ($kv.Value.IsPresent) { $argList += "-$($kv.Key)" }
        }
        else {
            $argList += "-$($kv.Key)"
            $argList += "`"$($kv.Value)`""
        }
    }

    Write-Warn2 'Administrator rights are required. Relaunching elevated...'
    try {
        Start-Process -FilePath (Get-Process -Id $PID).Path -ArgumentList $argList -Verb RunAs | Out-Null
    }
    catch {
        throw "Elevation was declined or failed. Re-run this script from an elevated PowerShell prompt. ($($_.Exception.Message))"
    }
}

function Get-SdbInstPath {
    # A 32-bit PowerShell process on 64-bit Windows gets redirected to SysWOW64,
    # where sdbinst cannot manage the 64-bit shim store. Sysnative bypasses that.
    if ([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess) {
        $native = Join-Path $env:SystemRoot 'Sysnative\sdbinst.exe'
        if (Test-Path -LiteralPath $native) { return $native }
    }
    $std = Join-Path $env:SystemRoot 'System32\sdbinst.exe'
    if (-not (Test-Path -LiteralPath $std)) {
        throw "sdbinst.exe not found. This script requires a standard Windows installation."
    }
    return $std
}

function Invoke-SdbInst {
    param([string[]]$Arguments)

    $exe = Get-SdbInstPath
    $out = Join-Path $env:TEMP "sdbinst-$PID.log"

    $p = Start-Process -FilePath $exe -ArgumentList $Arguments -Wait -PassThru `
                       -WindowStyle Hidden -RedirectStandardOutput $out
    $text = ''
    if (Test-Path -LiteralPath $out) {
        $text = (Get-Content -LiteralPath $out -Raw -ErrorAction SilentlyContinue)
        Remove-Item -LiteralPath $out -Force -ErrorAction SilentlyContinue
    }

    if ($p.ExitCode -ne 0) {
        throw "sdbinst failed with exit code $($p.ExitCode). $text"
    }
    return $text
}

#endregion

#region ---------------------------------------------------------------- discovery

function Resolve-GameFolder {
    param([string]$Explicit)

    $candidates = New-Object System.Collections.Generic.List[string]

    if ($Explicit) { $candidates.Add($Explicit) }

    foreach ($key in @(
        "HKLM:\SOFTWARE\WOW6432Node\GOG.com\Games\$($script:GogGameId)",
        "HKLM:\SOFTWARE\GOG.com\Games\$($script:GogGameId)"
    )) {
        if (Test-Path -LiteralPath $key) {
            $props = Get-ItemProperty -LiteralPath $key -ErrorAction SilentlyContinue
            if ($props -and $props.PSObject.Properties.Name -contains 'path' -and $props.path) {
                $candidates.Add([string]$props.path)
            }
        }
    }

    $candidates.Add('C:\GOG Games\Beyond Good and Evil')
    foreach ($pf in @($env:ProgramFiles, ${env:ProgramFiles(x86)})) {
        if ($pf) {
            $candidates.Add((Join-Path $pf 'GOG Galaxy\Games\Beyond Good and Evil'))
            $candidates.Add((Join-Path $pf 'GOG.com\Beyond Good and Evil'))
        }
    }

    foreach ($c in $candidates) {
        if (-not $c) { continue }
        $trimmed = $c.TrimEnd('\')
        if (Test-Path -LiteralPath (Join-Path $trimmed 'BGE.exe')) { return $trimmed }
    }

    if ($Explicit) {
        throw "BGE.exe was not found in '$Explicit'. Point -GamePath at the folder containing BGE.exe."
    }
    throw "Could not locate the Beyond Good & Evil install folder. Re-run with -GamePath '<folder containing BGE.exe>'."
}

function Get-SdbShimNames {
    # Shim names are stored as UTF-16LE strings in the .sdb string table. We only
    # need to identify the database, not parse the tagged binary format.
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) { return @() }

    try   { $bytes = [System.IO.File]::ReadAllBytes($Path) }
    catch { return @() }

    $text  = [System.Text.Encoding]::Unicode.GetString($bytes)
    $found = New-Object System.Collections.Generic.List[string]
    foreach ($shim in @($script:BadShim, $script:GoodShim)) {
        if ($text -match [regex]::Escape($shim)) { $found.Add($shim) }
    }
    return $found.ToArray()
}

function Get-BgeShimDatabase {
    <#
        Walks AppCompatFlags\Custom\BGE.exe -> {guid}.sdb -> InstalledSDB\{guid}
        rather than hardcoding a GUID, so this keeps working if GOG reissues the
        database. Returns $null when no matching database is installed.
    #>
    $exeKey = Join-Path $script:CustomKey 'BGE.exe'
    if (-not (Test-Path -LiteralPath $exeKey)) { return $null }

    $names = @()
    try { $names = (Get-Item -LiteralPath $exeKey).GetValueNames() } catch { return $null }

    $found = New-Object System.Collections.Generic.List[object]

    foreach ($valueName in $names) {
        if ($valueName -notmatch '^\{(?<g>[0-9a-fA-F-]{36})\}\.sdb$') { continue }
        $guid = $Matches['g']

        $instKey = Join-Path $script:InstalledKey "{$guid}"
        if (-not (Test-Path -LiteralPath $instKey)) { continue }

        $inst = Get-ItemProperty -LiteralPath $instKey -ErrorAction SilentlyContinue
        if (-not $inst) { continue }

        $dbPath = ''
        $dbDesc = ''
        if ($inst.PSObject.Properties.Name -contains 'DatabasePath')        { $dbPath = [string]$inst.DatabasePath }
        if ($inst.PSObject.Properties.Name -contains 'DatabaseDescription') { $dbDesc = [string]$inst.DatabaseDescription }

        $shims = @(Get-SdbShimNames -Path $dbPath)

        $found.Add([pscustomobject]@{
            Guid        = "{$guid}"
            Path        = $dbPath
            Description = $dbDesc
            Shims       = $shims
            HasBadShim  = ($shims -contains $script:BadShim)
        })
    }

    if ($found.Count -eq 0) { return $null }

    # Prefer a database we positively identified. If none matches, return the first
    # anyway so the caller's safety check trips and refuses to remove it blindly.
    foreach ($candidate in $found) {
        if ($candidate.HasBadShim) { return $candidate }
    }
    return $found[0]
}

#endregion

#region ---------------------------------------------------------------- state

function Get-State {
    if (-not (Test-Path -LiteralPath $script:StateFile)) { return $null }
    try   { return (Get-Content -LiteralPath $script:StateFile -Raw | ConvertFrom-Json) }
    catch { return $null }
}

function Save-State {
    param([hashtable]$State)
    if (-not (Test-Path -LiteralPath $script:StateDir)) {
        New-Item -ItemType Directory -Path $script:StateDir -Force | Out-Null
    }
    $json = $State | ConvertTo-Json -Depth 6
    [System.IO.File]::WriteAllText($script:StateFile, $json, (New-Object System.Text.UTF8Encoding($false)))
}

function Backup-File {
    param([string]$Path, [string]$Label)

    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    if (-not (Test-Path -LiteralPath $script:BackupDir)) {
        New-Item -ItemType Directory -Path $script:BackupDir -Force | Out-Null
    }
    $dest = Join-Path $script:BackupDir $Label
    Copy-Item -LiteralPath $Path -Destination $dest -Force
    return $dest
}

#endregion

#region ---------------------------------------------------------------- actions

function Get-LaunchTarget {
    param([string]$Folder)
    foreach ($exe in $script:LaunchChain) {
        $full = Join-Path $Folder $exe
        if (Test-Path -LiteralPath $full) { return $full }
    }
    return (Join-Path $Folder 'BGE.exe')
}

function New-AffinityShortcut {
    param([string]$Folder, [int]$Mask)

    $target   = Get-LaunchTarget -Folder $Folder
    $exeName  = Split-Path $target -Leaf
    $maskHex  = '{0:X}' -f $Mask

    $desktop  = [Environment]::GetFolderPath('Desktop')
    $linkPath = Join-Path $desktop $script:ShortcutName

    # cmd's `start /affinity` is the only built-in way to set affinity at launch.
    # The first quoted token is start's window title, not the program - it must be
    # present or start would treat the path as the title. Keep it free of '&', which
    # cmd treats as a command separator even inside a /c argument string.
    $arguments = '/c start "BGE" /affinity {0} /d "{1}" "{2}"' -f $maskHex, $Folder, $exeName

    $shell = New-Object -ComObject WScript.Shell
    try {
        $sc = $shell.CreateShortcut($linkPath)
        $sc.TargetPath       = Join-Path $env:SystemRoot 'System32\cmd.exe'
        $sc.Arguments        = $arguments
        $sc.WorkingDirectory = $Folder
        $sc.IconLocation     = "$(Join-Path $Folder 'BGE.exe'),0"
        $sc.WindowStyle      = 7   # start minimised so the cmd host does not flash
        $sc.Description      = 'Beyond Good & Evil with Alt+Tab enabled and single-core affinity'
        $sc.Save()
    }
    finally {
        [void][Runtime.InteropServices.Marshal]::ReleaseComObject($shell)
    }

    return [pscustomobject]@{ Path = $linkPath; Target = $exeName; Mask = $Mask }
}

function Test-Pe32 {
    # BGE.exe is a 32-bit process and silently fails to load a 64-bit DLL, so verify
    # the machine type rather than trusting the build produced what we expect.
    param([string]$Path)
    try {
        $fs = [System.IO.File]::OpenRead($Path)
        try {
            $br = New-Object System.IO.BinaryReader($fs)
            $fs.Position = 0x3C
            $peOff = $br.ReadInt32()
            if ($peOff -le 0 -or $peOff -gt ($fs.Length - 6)) { return $false }
            $fs.Position = $peOff + 4
            return ($br.ReadUInt16() -eq 0x14C)
        }
        finally { $fs.Dispose() }
    }
    catch { return $false }
}

function Install-WindowedProxy {
    <#
        Installs the in-repo d3d9 proxy, which forces the game out of exclusive
        fullscreen so Alt+Tab is safe. Self-contained: no third-party download.

        Only one file can be named d3d9.dll. If the game folder already has one from
        another project (dgVoodoo, DXVK, ReShade), it is renamed to
        d3d9_chain.dll and our proxy forwards to it rather than replacing it.
    #>
    param([string]$Folder)

    $proxy = $ProxyPath
    if (-not $proxy) { $proxy = Join-Path $PSScriptRoot 'dist\d3d9.dll' }

    if (-not (Test-Path -LiteralPath $proxy)) {
        Write-Warn2 "Proxy DLL not found: $proxy"
        Write-Info  'Build it first:   src\build.cmd'
        Write-Info  'Or pass an existing build with -ProxyPath.'
        return $null
    }
    if (-not (Test-Pe32 -Path $proxy)) {
        throw "$proxy is not a 32-bit DLL. BGE.exe is a 32-bit process and cannot load it. Rebuild with src\build.cmd."
    }

    $target  = Join-Path $Folder 'd3d9.dll'
    $chained = ''

    if (Test-Path -LiteralPath $target) {
        $have = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
        $ours = (Get-FileHash -LiteralPath $proxy  -Algorithm SHA256).Hash

        if ($have -eq $ours) {
            Write-Ok 'Proxy already installed and current'
        }
        else {
            $chainTarget = Join-Path $Folder 'd3d9_chain.dll'
            if (Test-Path -LiteralPath $chainTarget) {
                throw ("Both d3d9.dll and d3d9_chain.dll already exist in the game folder. " +
                       "Resolve that by hand so nothing is lost, then re-run.")
            }
            Backup-File -Path $target -Label 'd3d9-preexisting.dll' | Out-Null
            Move-Item -LiteralPath $target -Destination $chainTarget -Force
            $chained = $chainTarget
            Write-Ok 'Existing d3d9.dll renamed to d3d9_chain.dll; the proxy will forward to it'
        }
    }

    Copy-Item -LiteralPath $proxy -Destination $target -Force

    $ini = Join-Path $Folder 'd3d9_windowed.ini'
    $desc = @('windowed with a title bar', 'borderless, centred at game resolution',
              'borderless, stretched to fill the monitor')[$WindowMode]
    $lines = @(
        '; d3d9_windowed - forces the game out of exclusive fullscreen so Alt+Tab is safe.',
        '; Mode 0 = windowed   1 = borderless centred (default)   2 = borderless stretched',
        '[Display]',
        "Mode=$WindowMode",
        'Log=0',
        'Chain=d3d9_chain.dll'
    )
    [System.IO.File]::WriteAllLines($ini, $lines, (New-Object System.Text.UTF8Encoding($false)))

    Write-Ok "Windowed proxy installed (mode $WindowMode - $desc)"
    Write-Info "d3d9.dll  -> $target"
    Write-Info "config    -> $ini"

    return [pscustomobject]@{ Target = $target; Ini = $ini; Chained = $chained }
}

#endregion

#region ---------------------------------------------------------------- modes

function Show-Status {
    param([string]$Folder)

    Write-Head 'Current state'

    Write-Host "  Game folder    : $Folder"
    $exe = Join-Path $Folder 'BGE.exe'
    if (Test-Path -LiteralPath $exe) {
        $size = (Get-Item -LiteralPath $exe).Length
        $note = 'unrecognised build'
        if ($size -eq 7778304) { $note = 'GOG.com / Buka build' }
        Write-Host ("  BGE.exe        : {0:N0} bytes ($note)" -f $size)
    }

    $db = Get-BgeShimDatabase
    if ($db) {
        Write-Bad "Shim database INSTALLED - Alt+Tab is blocked"
        Write-Info "GUID  : $($db.Guid)"
        Write-Info "Desc  : $($db.Description)"
        Write-Info "Path  : $($db.Path)"
        if ($db.Shims.Count -gt 0) { Write-Info "Shims : $($db.Shims -join ', ')" }
        else { Write-Info "Shims : (could not read - file may be unreadable)" }
    }
    else {
        Write-Ok 'No BGE.exe shim database installed - Alt+Tab is not being blocked'
    }

    $link = Join-Path ([Environment]::GetFolderPath('Desktop')) $script:ShortcutName
    if (Test-Path -LiteralPath $link) { Write-Ok  "Affinity launcher present: $link" }
    else                              { Write-Warn2 'Affinity launcher not present' }

    $proxyIni = Join-Path $Folder 'd3d9_windowed.ini'
    if (Test-Path -LiteralPath $proxyIni) {
        $modeLine = Select-String -LiteralPath $proxyIni -Pattern '^\s*Mode\s*=' -ErrorAction SilentlyContinue
        $modeTxt = 'Mode unknown'
        if ($modeLine) { $modeTxt = $modeLine.Line.Trim() }
        Write-Ok "Windowed proxy installed ($modeTxt) - exclusive fullscreen disabled"
        if (Test-Path -LiteralPath (Join-Path $Folder 'd3d9_chain.dll')) {
            Write-Info 'Chaining to d3d9_chain.dll'
        }
    }
    else {
        Write-Warn2 'Windowed proxy not installed'
    }

    $state = Get-State
    if ($state) { Write-Info "Fix applied on: $($state.AppliedUtc) UTC" }
    Write-Host ''
}

function Invoke-Apply {
    param([string]$Folder)

    Write-Head 'Plan'

    $db = Get-BgeShimDatabase
    if ($db -and -not $db.HasBadShim) {
        throw ("A shim database is registered for BGE.exe but does not contain '$($script:BadShim)'. " +
               "Refusing to remove a database this script did not identify. Inspect: $($db.Path)")
    }

    $steps = New-Object System.Collections.Generic.List[string]
    if ($db)            { $steps.Add("Remove shim database $($db.Guid) ('$($db.Description)')") }
    else                { $steps.Add('Shim database already absent - nothing to remove') }
    if (-not $NoShortcut) { $steps.Add("Create affinity launcher shortcut (mask $AffinityMask)") }
    if (-not $NoWindowedProxy) { $steps.Add("Install d3d9 windowed proxy (window mode $WindowMode)") }

    $i = 1
    foreach ($s in $steps) { Write-Host "  $i. $s"; $i++ }
    Write-Host ''
    Write-Info "Backups and revert data: $script:StateDir"
    Write-Host ''

    if (-not $Force -and -not $WhatIfPreference) {
        $answer = Read-Host '  Proceed? [y/N]'
        if ($answer -notmatch '^(?i:y|yes)$') {
            Write-Warn2 'Aborted. Nothing was changed.'
            return
        }
    }

    Write-Head 'Applying'

    $state = @{
        AppliedUtc     = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
        GamePath       = $Folder
        ShimGuid       = ''
        ShimBackup     = ''
        ShimSource     = ''
        ShortcutPath   = ''
        AffinityMask   = $AffinityMask
        ProxyTarget    = ''
        ProxyIni       = ''
        ProxyChained   = ''
    }

    # ---- 1. shim database ------------------------------------------------
    if ($db) {
        Write-Step "Backing up $($db.Path)"
        $backup = $null
        if ($PSCmdlet.ShouldProcess($db.Path, 'Back up shim database')) {
            $backup = Backup-File -Path $db.Path -Label 'goggame-installed.sdb'
        }

        # The game folder also ships an original copy; keep it as a second source.
        $shipped = Join-Path $Folder 'goggame.sdb'
        if (Test-Path -LiteralPath $shipped) {
            if ($PSCmdlet.ShouldProcess($shipped, 'Back up shipped shim database')) {
                Backup-File -Path $shipped -Label 'goggame-shipped.sdb' | Out-Null
            }
            $state.ShimSource = $shipped
        }

        if ($PSCmdlet.ShouldProcess($db.Guid, 'Uninstall shim database via sdbinst')) {
            Write-Step "Uninstalling shim database $($db.Guid)"
            Invoke-SdbInst -Arguments @('-q', '-g', $db.Guid) | Out-Null

            if (Get-BgeShimDatabase) {
                throw 'sdbinst reported success but the database is still registered. Nothing else was changed.'
            }
            Write-Ok "Removed '$($script:BadShim)' - Alt+Tab is no longer intercepted"
        }

        $state.ShimGuid   = $db.Guid
        if ($backup) { $state.ShimBackup = $backup }
    }
    else {
        Write-Ok 'No shim database registered for BGE.exe - skipping'
    }

    # ---- 2. affinity launcher -------------------------------------------
    if (-not $NoShortcut) {
        if ($PSCmdlet.ShouldProcess('Desktop shortcut', 'Create affinity launcher')) {
            $sc = New-AffinityShortcut -Folder $Folder -Mask $AffinityMask
            Write-Ok "Launcher created: $($sc.Path)"
            Write-Info "Launches $($sc.Target) with affinity mask $($sc.Mask); BGE.exe inherits it."
            $state.ShortcutPath = $sc.Path
        }
    }
    else {
        Write-Warn2 "Skipped launcher. Without it, '$($script:GoodShim)' is not replaced."
    }

    # ---- 3. windowed proxy ----------------------------------------------
    if (-not $NoWindowedProxy) {
        if ($PSCmdlet.ShouldProcess($Folder, 'Install d3d9 windowed proxy')) {
            $px = Install-WindowedProxy -Folder $Folder
            if ($px) {
                $state.ProxyTarget  = $px.Target
                $state.ProxyIni     = $px.Ini
                $state.ProxyChained = $px.Chained
            }
        }
    }
    else {
        Write-Warn2 'Proxy skipped - the game keeps an exclusive fullscreen device.'
        Write-Info  'Alt+Tab will work, but the HUD can corrupt when you return.'
    }

    if (-not $WhatIfPreference) {
        Save-State -State $state
        Write-Host ''
        Write-Ok 'Done.'
        if ($state.ShortcutPath) { Write-Info 'Launch the game from the new desktop shortcut to keep single-core affinity.' }
        Write-Info "Undo with: .\Fix-BGEAltTab.ps1 -Revert"
        Write-Host ''
    }
}

function Invoke-Revert {
    Write-Head 'Reverting'

    $state = Get-State
    if (-not $state) {
        Write-Warn2 'No saved state found. Attempting a best-effort revert.'
    }

    # ---- shim database ---------------------------------------------------
    if (-not (Get-BgeShimDatabase)) {
        $source = ''
        foreach ($candidate in @(
            (Join-Path $script:BackupDir 'goggame-installed.sdb'),
            (Join-Path $script:BackupDir 'goggame-shipped.sdb')
        )) {
            if (Test-Path -LiteralPath $candidate) { $source = $candidate; break }
        }

        if (-not $source -and $state -and $state.GamePath) {
            $shipped = Join-Path $state.GamePath 'goggame.sdb'
            if (Test-Path -LiteralPath $shipped) { $source = $shipped }
        }

        if ($source) {
            if ($PSCmdlet.ShouldProcess($source, 'Reinstall shim database')) {
                Write-Step "Reinstalling $source"
                Invoke-SdbInst -Arguments @('-q', $source) | Out-Null
                Write-Ok 'Original shim database reinstalled (Alt+Tab blocked again)'
            }
        }
        else {
            Write-Warn2 'No backup of the shim database found - cannot reinstall it.'
            Write-Info 'Reinstalling the game through GOG restores it.'
        }
    }
    else {
        Write-Ok 'Shim database already installed - nothing to restore'
    }

    # ---- shortcut --------------------------------------------------------
    $link = Join-Path ([Environment]::GetFolderPath('Desktop')) $script:ShortcutName
    if ($state -and $state.ShortcutPath) { $link = $state.ShortcutPath }
    if (Test-Path -LiteralPath $link) {
        if ($PSCmdlet.ShouldProcess($link, 'Remove launcher shortcut')) {
            Remove-Item -LiteralPath $link -Force
            Write-Ok 'Launcher shortcut removed'
        }
    }

    # ---- windowed proxy --------------------------------------------------
    # If the proxy displaced an existing d3d9.dll it was renamed to d3d9_chain.dll;
    # putting it back is part of leaving the folder as we found it.
    if ($state) {
        $pTarget  = ''
        $pIni     = ''
        $pChained = ''
        if ($state.PSObject.Properties.Name -contains 'ProxyTarget')  { $pTarget  = [string]$state.ProxyTarget }
        if ($state.PSObject.Properties.Name -contains 'ProxyIni')     { $pIni     = [string]$state.ProxyIni }
        if ($state.PSObject.Properties.Name -contains 'ProxyChained') { $pChained = [string]$state.ProxyChained }

        if ($pTarget -and (Test-Path -LiteralPath $pTarget)) {
            if ($PSCmdlet.ShouldProcess($pTarget, 'Remove windowed proxy')) {
                Remove-Item -LiteralPath $pTarget -Force
                if ($pIni -and (Test-Path -LiteralPath $pIni)) { Remove-Item -LiteralPath $pIni -Force }
                Write-Ok 'Windowed proxy removed'

                if ($pChained -and (Test-Path -LiteralPath $pChained)) {
                    Move-Item -LiteralPath $pChained -Destination $pTarget -Force
                    Write-Ok 'Restored the previous d3d9.dll from d3d9_chain.dll'
                }
            }
        }
    }

    if (-not $WhatIfPreference -and (Test-Path -LiteralPath $script:StateFile)) {
        Remove-Item -LiteralPath $script:StateFile -Force
    }

    Write-Host ''
    Write-Ok 'Revert complete.'
    Write-Host ''
}

#endregion

#region ---------------------------------------------------------------- entry point

try {
    Write-Host ''
    Write-Host "  $script:AppName" -ForegroundColor White
    Write-Host '  Restores Alt+Tab in the GOG release of Beyond Good & Evil (2003)' -ForegroundColor DarkGray

    $folder = Resolve-GameFolder -Explicit $GamePath

    if ($Status) {
        Show-Status -Folder $folder
        return
    }

    if (-not (Test-Admin)) {
        if ($WhatIfPreference) {
            # A preview writes nothing, and every read it performs works unelevated,
            # so do not force a UAC prompt just to show the plan.
            Write-Warn2 'Not elevated: preview only. The real run will request elevation.'
        }
        elseif ($NoElevate) {
            throw 'Administrator rights are required (sdbinst modifies machine-scope settings). Re-run from an elevated prompt.'
        }
        else {
            Invoke-SelfElevate
            return
        }
    }

    $running = @(Get-Process -Name 'BGE', 'CheckApplication' -ErrorAction SilentlyContinue)
    if ($running.Count -gt 0) {
        Write-Warn2 'Beyond Good & Evil appears to be running. Close it before continuing.'
    }

    if ($Revert) { Invoke-Revert }
    else         { Invoke-Apply -Folder $folder }
}
catch {
    Write-Host ''
    Write-Bad $_.Exception.Message
    Write-Host ''
    exit 1
}

#endregion
