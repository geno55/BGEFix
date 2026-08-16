<#
.SYNOPSIS
    BGEFix - fixes for the GOG.com release of Beyond Good & Evil (2003).

.DESCRIPTION
    Three independent fixes, selected with -Component:

    AltTab
        The GOG release installs a custom Windows Application Compatibility shim
        database (shipped as 'goggame.sdb' in the game folder). It applies two
        shims to BGE.exe:

            IgnoreAltTab        - makes the game swallow Alt+Tab entirely.
            SingleProcAffinity  - pins the game to a single CPU core, which keeps
                                  the Jade engine's timing stable on modern CPUs.

        The first is why you cannot Alt+Tab. The second is a fix you actually
        want. Because a custom shim database is all-or-nothing, this script
        removes GOG's with Microsoft's supported tool (sdbinst) and installs a
        replacement derived from it that applies only SingleProcAffinity - so
        affinity stays enforced by Windows however the game is launched.

    Windowed
        Installs the bundled d3d9 proxy (dist\d3d9.dll), which forces the game out
        of exclusive fullscreen. Without it the game holds an exclusive Direct3D 9
        device, and alt-tabbing away can corrupt the HUD or crash on return -
        which is the bug GOG papered over by disabling Alt+Tab in the first place.

    Controller
        Installs the bundled dinput8 proxy (dist\dinput8.dll), which adds XInput
        (Xbox) gamepad support. The GOG build has no gamepad code at all. Nothing
        to do with Alt+Tab, and installable on its own.

    Nothing is added to your desktop, and it does not matter how you launch the game:
    both fixes are properties of BGE.exe applied by Windows itself.

    Every change is backed up and reversible with -Revert.

.PARAMETER GamePath
    Path to the Beyond Good & Evil install folder. Auto-detected from the GOG
    registry entry or common install locations when omitted.

.PARAMETER Revert
    Undo changes: restore GOG's original shim database and remove the proxies.
    Narrow it with -Component.

.PARAMETER Status
    Report the current state and exit without changing anything.

.PARAMETER Component
    Which parts to install, or with -Revert, which parts to undo. One list, one
    polarity, and every part is independent:

        AltTab      Remove GOG's shim database and install a replacement that applies
                    only SingleProcAffinity. This is the Alt+Tab fix.
        Windowed    The d3d9 proxy that takes the game out of exclusive fullscreen.
        Controller  The dinput8 proxy that adds XInput gamepad support. Unrelated to
                    Alt+Tab, and installable on its own: -Component Controller.
        All         All three.

    Default when installing: AltTab, Windowed. Default when reverting: All.

.PARAMETER ProxyPath
    Path to a prebuilt proxy DLL. Defaults to dist\d3d9.dll beside this script.
    Must be 32-bit; this is verified before installing.

.PARAMETER WindowMode
    0 = windowed with a title bar, 1 = borderless centred at the game's resolution
    (default), 2 = borderless stretched to fill the monitor.

.PARAMETER PadPath
    Path to a prebuilt dinput8 proxy. Defaults to dist\dinput8.dll beside this script.

.PARAMETER PadLookSpeed
    Right-stick look speed in mouse counts per second at full deflection, 100-20000.
    Default 1800. This is a real unit: the camera turns at the same rate whatever
    frame rate the game runs at.

    Replaces -PadLookSensitivity, which was counts per *poll* and so meant something
    different on every machine. Use -ResetConfig to rewrite an older ini.

.PARAMETER PadDeadzone
    Left-stick deadzone in raw XInput units, 0-32000. Default 7849, XInput's own
    recommended value. Applied radially, so the dead region is a circle.

.PARAMETER PadLookDeadzone
    Right-stick deadzone in raw XInput units, 0-32000. Default 8689, XInput's own
    recommended value for the right stick.

.PARAMETER PadInvertLook
    Invert the right stick's vertical look axis.

.PARAMETER ResetConfig
    Regenerate d3d9_windowed.ini and dinput8_xinput.ini from the current options.
    Without this, an existing config is left alone so re-running never discards your
    remapping.

.PARAMETER NoElevate
    Fail with an error instead of relaunching elevated when not running as admin.

.PARAMETER Force
    Skip the interactive confirmation prompt.

.EXAMPLE
    .\Fix-BGE.ps1 -Status

.EXAMPLE
    .\Fix-BGE.ps1

.EXAMPLE
    .\Fix-BGE.ps1 -GamePath "D:\Games\Beyond Good and Evil" -Force

.EXAMPLE
    .\Fix-BGE.ps1 -WindowMode 2 -Force

.EXAMPLE
    # Gamepad support only - touches nothing else.
    .\Fix-BGE.ps1 -Component Controller

.EXAMPLE
    # Everything, including the controller proxy.
    .\Fix-BGE.ps1 -Component All

.EXAMPLE
    # Undo just the controller proxy, leave the rest installed.
    .\Fix-BGE.ps1 -Revert -Component Controller

.EXAMPLE
    .\Fix-BGE.ps1 -Revert

.NOTES
    Requires administrator rights (sdbinst modifies machine-scope compatibility
    settings). Tested against the GOG build with BGE.exe at 7,778,304 bytes.

    License: MIT. Not affiliated with GOG.com or Ubisoft.
#>
# Note: #Requires must stay below the help block. Placed above it, PowerShell stops
# recognising the block as comment-based help and Get-Help falls back to bare syntax.
#Requires -Version 5.1
#
# Parameter sets exist so that contradictory invocations fail at binding time instead of
# quietly doing something else. -Status -Revert used to run Status and drop the Revert;
# -Revert -WindowMode 2 used to accept a setting for work it was not doing. Both are now
# errors PowerShell raises before a line of this script runs.
#
# Feature selection is one -Component list with one polarity. It replaces -NoShortcut,
# -NoWindowedProxy and -InstallControllerSupport, which were two opt-outs and an opt-in
# for three co-equal features - exactly the kind of thing people get backwards. Those
# switches are gone rather than deprecated: passing one is now a parameter binding error,
# which is loud and changes nothing, where a silent remap would not be.
#
[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Medium', DefaultParameterSetName = 'Apply')]
param(
    [Parameter(ParameterSetName = 'Apply')]
    [Parameter(ParameterSetName = 'Status')]
    [Parameter(ParameterSetName = 'Revert')]
    [string]   $GamePath,

    # Which parts to act on. Same list for installing and reverting, one polarity.
    #   AltTab     - remove GOG's shim database, install the affinity-only replacement
    #   Windowed   - the d3d9 windowed proxy
    #   Controller - the dinput8 XInput proxy (independent of everything else)
    [Parameter(ParameterSetName = 'Apply')]
    [Parameter(ParameterSetName = 'Revert')]
    [ValidateSet('AltTab', 'Windowed', 'Controller', 'All')]
    [string[]] $Component,

    [Parameter(ParameterSetName = 'Status', Mandatory = $true)]
    [switch]   $Status,

    [Parameter(ParameterSetName = 'Revert', Mandatory = $true)]
    [switch]   $Revert,

    # --- apply-only tuning. Absent from Status and Revert, so passing one there is an error.
    [Parameter(ParameterSetName = 'Apply')]
    [string]   $ProxyPath,

    [Parameter(ParameterSetName = 'Apply')]
    [ValidateRange(0, 2)]
    [int]      $WindowMode = 1,

    [Parameter(ParameterSetName = 'Apply')]
    [string]   $PadPath,

    [Parameter(ParameterSetName = 'Apply')]
    [ValidateRange(100, 20000)]
    [int]      $PadLookSpeed = 1800,

    [Parameter(ParameterSetName = 'Apply')]
    [ValidateRange(0, 32000)]
    [int]      $PadDeadzone = 7849,

    [Parameter(ParameterSetName = 'Apply')]
    [ValidateRange(0, 32000)]
    [int]      $PadLookDeadzone = 8689,

    [Parameter(ParameterSetName = 'Apply')]
    [switch]   $PadInvertLook,

    [Parameter(ParameterSetName = 'Apply')]
    [switch]   $ResetConfig,

    # --- everywhere
    [switch]   $NoElevate,
    [switch]   $Force,

    # Internal. Set when this script relaunched itself elevated, so the child knows to
    # pause before its console closes rather than vanishing with the summary.
    [switch]   $ElevatedRelaunch
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

#region ---------------------------------------------------------------- constants

$script:AppName      = 'BGEFix'
$script:StateDir     = Join-Path $env:ProgramData 'BGEFix'
$script:BackupDir    = Join-Path $script:StateDir  'backup'
$script:StateFile    = Join-Path $script:StateDir  'state.json'


# The shim we are targeting, and the one we must preserve the effect of.
$script:BadShim      = 'IgnoreAltTab'
$script:GoodShim     = 'SingleProcAffinity'

# GOG product id for Beyond Good & Evil.
$script:GogGameId    = '1207658746'

$script:CustomKey    = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Custom'
$script:InstalledKey = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\InstalledSDB'

# Embedded in dist\d3d9.dll and dist\dinput8.dll by src\*.cpp, so the installer can
# recognise ANY build of its own proxies rather than just the one it shipped with.
#
# The GUID is the identity and must never change. The trailing number is a version the
# installer PARSES; it must never compare the marker as a whole. An installer that looked
# for the exact string it ships would meet the previous release's DLL, classify it as a
# third-party wrapper, rename it to *_chain.dll and chain itself to it - which is exactly
# the failure the marker exists to prevent, arriving one release later on schedule.
$script:ProxyMarkerPrefix  = 'BGEFIX_PROXY{502eb6b9-f979-4627-b242-8e146e0fc1de}v'
$script:ProxyMarkerVersion = 2

# Markers from before the versioned format existed. Recognised so that upgrading does not
# treat our own earlier build as somebody else's wrapper - the same trap, in reverse.
$script:LegacyProxyMarkers = @('BGEFIX_PROXY_V1')

# The replacement shim database: GOG's own, minus IgnoreAltTab. Fixed GUID so re-running
# replaces our previous install rather than accumulating registrations, and a distinct
# name so it is obvious in -Status and in Installed Programs whose database it is.
$script:AffinityDbGuid = [guid]'7b3f4d21-9c58-4a06-9e77-1d2b8a5f6c34'
$script:AffinityDbName = 'BGEFix (SingleProcAffinity only)'
$script:AffinityDbFile = 'bge_affinity_only.sdb'

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

function Resolve-Components {
    <#
        Works out which parts to act on and returns them as a set.

        Installing defaults to the Alt+Tab fix and the windowed proxy, but not controller
        support - that one is opt-in because it is unrelated to the rest. -Component All
        adds it. Reverting defaults to everything, because a partial revert has to be
        asked for.
    #>
    param([string[]]$Requested, [switch]$ForRevert)

    $all = @('AltTab', 'Windowed', 'Controller')

    if ($Requested) {
        if ($Requested -contains 'All') { return $all }
        return @($Requested | Select-Object -Unique)
    }

    if ($ForRevert) { return $all }
    return @('AltTab', 'Windowed')
}

function Assert-ComponentOptions {
    <#
        Rejects tuning that applies to a component not being installed.

        Parameter sets cannot express "only meaningful when -Component includes Windowed",
        so this covers what they cannot: -Component Controller -WindowMode 2 is a setting
        for something this run is not touching, and silently ignoring it is how people
        come to believe they configured something they did not.
    #>
    param([string[]]$Components, [System.Collections.IDictionary]$Bound)

    $rules = @(
        @{ Params = @('WindowMode', 'ProxyPath');                                        Needs = 'Windowed' }
        @{ Params = @('PadPath', 'PadLookSpeed', 'PadDeadzone', 'PadLookDeadzone', 'PadInvertLook'); Needs = 'Controller' }
    )

    foreach ($rule in $rules) {
        if ($Components -contains $rule.Needs) { continue }
        foreach ($p in $rule.Params) {
            if ($Bound.ContainsKey($p)) {
                throw ("-$p only applies to the $($rule.Needs) component, which this run is not " +
                       "installing (-Component $($Components -join ',')). Add $($rule.Needs) or drop -$p.")
            }
        }
    }

    if ($Bound.ContainsKey('ResetConfig') -and
        -not (($Components -contains 'Windowed') -or ($Components -contains 'Controller'))) {
        throw ('-ResetConfig regenerates the proxy .ini files, and this run installs neither ' +
               'proxy. Add Windowed or Controller, or drop -ResetConfig.')
    }
}

function ConvertTo-ProcessArgument {
    <#
        Quotes one value for a Windows command line, following the rules
        CommandLineToArgvW actually implements rather than just wrapping it in quotes.

        The case that matters here is a trailing backslash. "D:\Games\BGE\" - exactly what
        drag-and-drop and tab completion produce - is parsed as an escaped quote, so the
        argument swallows the rest of the command line and -GamePath arrives mangled. The
        fix is that backslashes are only special immediately before a quote: 2n of them
        yield n literal backslashes and end the quoted run, so any run that would end up
        adjacent to our closing quote has to be doubled.
    #>
    param([string]$Value)

    if ($null -eq $Value) { return '""' }

    # Escape backslash runs that precede an embedded quote, then the quote itself.
    $escaped = [regex]::Replace($Value, '(\\*)"', '$1$1\"')
    # Escape a backslash run that would land against our own closing quote.
    $escaped = [regex]::Replace($escaped, '(\\+)$', '$1$1')

    return '"' + $escaped + '"'
}

function Invoke-SelfElevate {
    <#
        Rebuild the original invocation so the elevated copy does the same thing, run it,
        and return its exit code.

        $Bound must be passed in from the script scope. $PSBoundParameters is per-scope:
        read inside a function it is that function's own bound parameters, which for this
        one is always empty - so reading it here silently forwarded nothing and the
        elevated child ran with every option at its default.

        The elevated child is waited on and its exit code returned. Firing it off and
        returning immediately made the parent exit 0 whether the child succeeded, threw,
        or the user cancelled the UAC prompt - a guaranteed false success for anything
        scripting this.

        -NoExit is deliberately not used. It kept the summary readable but made -Force,
        the flag that exists for unattended use, leave a console open forever. The child
        is told it was relaunched instead, and pauses itself only when interactive.
    #>
    param([System.Collections.IDictionary]$Bound)

    $argList = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass',
        '-File', (ConvertTo-ProcessArgument $PSCommandPath)
    )

    if (-not $Bound) { $Bound = @{} }

    foreach ($kv in $Bound.GetEnumerator()) {
        if ($kv.Key -in @('NoElevate', 'ElevatedRelaunch')) { continue }
        if ($kv.Value -is [switch]) {
            if ($kv.Value.IsPresent) { $argList += "-$($kv.Key)" }
        }
        else {
            $argList += "-$($kv.Key)"
            $argList += (ConvertTo-ProcessArgument ([string]$kv.Value))
        }
    }

    $argList += '-ElevatedRelaunch'

    Write-Warn2 'Administrator rights are required. Relaunching elevated...'

    $proc = $null
    try {
        $proc = Start-Process -FilePath (Get-Process -Id $PID).Path `
                              -ArgumentList $argList -Verb RunAs -PassThru
    }
    catch {
        throw "Elevation was declined or failed. Re-run this script from an elevated PowerShell prompt. ($($_.Exception.Message))"
    }
    if (-not $proc) {
        throw 'Elevation did not start a process. Re-run this script from an elevated PowerShell prompt.'
    }

    $proc.WaitForExit()
    try   { $code = $proc.ExitCode }
    catch { $code = 0 }
    if ($null -eq $code) { $code = 0 }

    if ($code -ne 0) { Write-Bad "The elevated run failed (exit code $code)." }
    else             { Write-Ok  'Elevated run completed.' }
    return [int]$code
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

# Bind-once state for Initialize-SdbApi. Declared here rather than assigned on first use:
# this script runs under Set-StrictMode 2.0, where reading a variable that was never set
# is a terminating error, not $null.
$script:SdbApiReady = $false
$script:SdbApiTried = $false

function Initialize-SdbApi {
    <#
        Binds the shim-database reader in apphelp.dll. These exports are how the OS itself
        reads .sdb files, so the format cannot drift out from under us the way a private
        parser would. Returns $true when the binding is usable.
    #>
    if ($script:SdbApiReady) { return $true }
    if ($script:SdbApiTried) { return $false }
    $script:SdbApiTried = $true

    try {
        Add-Type -Namespace BgeFix -Name Sdb -ErrorAction Stop -MemberDefinition @'
[DllImport("apphelp.dll", CharSet = CharSet.Unicode, SetLastError = true)]
public static extern IntPtr SdbOpenDatabase(string path, int pathType);
[DllImport("apphelp.dll")]
public static extern void SdbCloseDatabase(IntPtr pdb);
[DllImport("apphelp.dll")]
public static extern uint SdbGetFirstChild(IntPtr pdb, uint parent);
[DllImport("apphelp.dll")]
public static extern uint SdbGetNextChild(IntPtr pdb, uint parent, uint prev);
[DllImport("apphelp.dll")]
public static extern ushort SdbGetTagFromTagID(IntPtr pdb, uint tagid);
[DllImport("apphelp.dll")]
public static extern IntPtr SdbGetStringTagPtr(IntPtr pdb, uint tagid);
'@
        $script:SdbApiReady = $true
    }
    catch {
        $script:SdbApiReady = $false
    }
    return $script:SdbApiReady
}

function Get-SdbTagString {
    param([IntPtr]$Pdb, [uint32]$TagId)
    $p = [BgeFix.Sdb]::SdbGetStringTagPtr($Pdb, $TagId)
    if ($p -eq [IntPtr]::Zero) { return '' }
    return [System.Runtime.InteropServices.Marshal]::PtrToStringUni($p)
}

function Get-SdbChildString {
    # First child of $Parent carrying $Tag, as a string. '' when absent.
    param([IntPtr]$Pdb, [uint32]$Parent, [uint16]$Tag)
    $c = [BgeFix.Sdb]::SdbGetFirstChild($Pdb, $Parent)
    while ($c -ne 0) {
        if ([BgeFix.Sdb]::SdbGetTagFromTagID($Pdb, $c) -eq $Tag) {
            return (Get-SdbTagString -Pdb $Pdb -TagId $c)
        }
        $c = [BgeFix.Sdb]::SdbGetNextChild($Pdb, $Parent, $c)
    }
    return ''
}

function Get-SdbContents {
    <#
        Enumerates what a shim database ACTUALLY contains, by walking its tag tree.

        Returns an object with:
            Ok      $true only if the database was parsed. $false means UNKNOWN, which is
                    not the same as empty, and callers must not treat it as "nothing here".
            Name    the database's own display name
            Entries one record per executable it patches: Exe, plus every modification
                    applied to it as { Kind; Name }
            Shims   flat list of every applied modification name, for convenience

        Why this is not a byte search: the previous version scanned the raw file for the
        two shim names this script already hardcodes, so it was structurally incapable of
        returning a third. If GOG reissued the database with an extra shim, the tool would
        report the same two, remove the database, and drop the third silently - while
        -Status printed the list as though it were an inventory. Removing a machine-scope
        compatibility database is only defensible if we know what is in it, so we read it.

        Modifications are collected as "every list child of an EXE entry that is not a
        matching-file rule", rather than by looking for known tags. A shim, a patch, a
        layer or a flag we have never heard of is therefore still reported, which is the
        entire point.
    #>
    param([string]$Path)

    $unknown = [pscustomobject]@{ Ok = $false; Name = ''; Entries = @(); Shims = @() }

    if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path -LiteralPath $Path)) { return $unknown }
    if (-not (Initialize-SdbApi)) { return $unknown }

    # Tag ids from the .sdb format. Type lives in the high nibble: 0x7000 = list.
    $TAG_DATABASE      = [uint16]0x7001
    $TAG_EXE           = [uint16]0x7007
    $TAG_MATCHING_FILE = [uint16]0x7008
    $TAG_NAME          = [uint16]0x6001

    $pdb = [IntPtr]::Zero
    try {
        $full = (Resolve-Path -LiteralPath $Path).ProviderPath
        $pdb  = [BgeFix.Sdb]::SdbOpenDatabase($full, 0)     # 0 = DOS path
        if ($pdb -eq [IntPtr]::Zero) { return $unknown }

        $dbName  = ''
        $entries = New-Object System.Collections.Generic.List[object]
        $shims   = New-Object System.Collections.Generic.List[string]

        $top = [BgeFix.Sdb]::SdbGetFirstChild($pdb, 0)
        while ($top -ne 0) {
            if ([BgeFix.Sdb]::SdbGetTagFromTagID($pdb, $top) -eq $TAG_DATABASE) {
                $dbName = Get-SdbChildString -Pdb $pdb -Parent $top -Tag $TAG_NAME

                $node = [BgeFix.Sdb]::SdbGetFirstChild($pdb, $top)
                while ($node -ne 0) {
                    if ([BgeFix.Sdb]::SdbGetTagFromTagID($pdb, $node) -eq $TAG_EXE) {
                        $exe  = Get-SdbChildString -Pdb $pdb -Parent $node -Tag $TAG_NAME
                        $mods = New-Object System.Collections.Generic.List[object]

                        $kid = [BgeFix.Sdb]::SdbGetFirstChild($pdb, $node)
                        while ($kid -ne 0) {
                            $kidTag = [BgeFix.Sdb]::SdbGetTagFromTagID($pdb, $kid)
                            # Any list under an EXE other than a matching-file rule is
                            # something being applied to that executable.
                            if ((($kidTag -band 0xF000) -eq 0x7000) -and $kidTag -ne $TAG_MATCHING_FILE) {
                                $modName = Get-SdbChildString -Pdb $pdb -Parent $kid -Tag $TAG_NAME
                                if ($modName) {
                                    $mods.Add([pscustomobject]@{
                                        Kind = ('0x{0:X4}' -f $kidTag)
                                        Name = $modName
                                    })
                                    $shims.Add($modName)
                                }
                            }
                            $kid = [BgeFix.Sdb]::SdbGetNextChild($pdb, $node, $kid)
                        }

                        $entries.Add([pscustomobject]@{ Exe = $exe; Mods = $mods.ToArray() })
                    }
                    $node = [BgeFix.Sdb]::SdbGetNextChild($pdb, $top, $node)
                }
            }
            $top = [BgeFix.Sdb]::SdbGetNextChild($pdb, 0, $top)
        }

        return [pscustomobject]@{
            Ok      = $true
            Name    = $dbName
            Entries = $entries.ToArray()
            Shims   = ($shims | Select-Object -Unique)
        }
    }
    catch { return $unknown }
    finally {
        if ($pdb -ne [IntPtr]::Zero) {
            try { [BgeFix.Sdb]::SdbCloseDatabase($pdb) } catch { }
        }
    }
}

function Get-SdbNodes {
    <#
        Walks a shim database's raw tag tree, returning a flat node list with parent links.

        Get-SdbContents reads databases through apphelp.dll, which is authoritative but
        gives no byte offsets or sizes. Rewriting one needs both, so this is a second,
        deliberately small walker used only for surgery. The two are cross-checked against
        each other in the tests: they must agree on which shims a database applies.

        Tag encoding: a WORD whose high nibble is the type. Fixed-size types carry their
        data inline; lists, strings and binaries carry a DWORD size first.
    #>
    param([byte[]]$Bytes)

    if ($null -eq $Bytes -or $Bytes.Length -lt 16) { return $null }

    $nodes = New-Object System.Collections.Generic.List[object]
    $stack = New-Object System.Collections.Generic.Stack[object]
    $stack.Push([pscustomobject]@{ Start = 12; End = $Bytes.Length; Parent = -1 })

    while ($stack.Count -gt 0) {
        $frame = $stack.Pop()
        $pos = $frame.Start
        while ($pos -lt $frame.End -and $pos + 2 -le $Bytes.Length) {
            $tag = [BitConverter]::ToUInt16($Bytes, $pos)
            if ($tag -eq 0) { break }
            switch ($tag -band 0xF000) {
                0x1000 { $hs = 2; $ds = 0 }
                0x2000 { $hs = 2; $ds = 1 }
                0x3000 { $hs = 2; $ds = 2 }
                0x4000 { $hs = 2; $ds = 4 }
                0x5000 { $hs = 2; $ds = 8 }
                0x6000 { $hs = 2; $ds = 4 }
                0x7000 { $hs = 6; $ds = [BitConverter]::ToInt32($Bytes, $pos + 2) }
                0x8000 { $hs = 6; $ds = [BitConverter]::ToInt32($Bytes, $pos + 2) }
                0x9000 { $hs = 6; $ds = [BitConverter]::ToInt32($Bytes, $pos + 2) }
                default { return $null }                       # unknown type: refuse
            }
            if ($ds -lt 0 -or $pos + $hs + $ds -gt $Bytes.Length) { return $null }

            $idx = $nodes.Count
            $nodes.Add([pscustomobject]@{
                Index = $idx; Pos = $pos; Tag = $tag
                HdrSize = $hs; DataSize = $ds; Parent = $frame.Parent
            })
            if (($tag -band 0xF000) -eq 0x7000) {
                $stack.Push([pscustomobject]@{ Start = $pos + $hs; End = $pos + $hs + $ds; Parent = $idx })
            }
            $pos += $hs + $ds
        }
    }
    return $nodes
}

function New-AffinityOnlySdb {
    <#
        Writes a copy of a shim database with one shim removed - used to turn GOG's
        database into one that applies SingleProcAffinity and nothing else.

        Why derive rather than ship a prebuilt .sdb: the parts that make a shim actually
        apply are GOG's, not ours - the matching rules, the app name and vendor, the
        per-executable GUID, and the name index keyed on them. Reusing their file minus
        one entry keeps all of it intact, works if GOG reissues the database, and avoids
        redistributing their data in this repository. Our own DLLs are ours to ship; this
        file is not.

        The rewrite is safe because of how the format is laid out:

          - A SHIM_REF is a self-contained list, so removing it is a splice.
          - STRINGREFs are offsets relative to the string table's own data start, so they
            stay valid as long as the table's internals do not move.
          - The name index stores absolute TAGIDs, so any that pointed past the splice are
            shifted back by the number of bytes removed.
          - Enclosing list sizes shrink by the same amount.

        Returns an object with Ok, and on success Shims (what the result applies).
    #>
    param(
        [string]$SourcePath,
        [string]$DestPath,
        [string]$RemoveShim,
        [guid]$NewGuid,
        [string]$NewName
    )

    $fail = [pscustomobject]@{ Ok = $false; Shims = @(); Reason = '' }

    if (-not (Test-Path -LiteralPath $SourcePath)) {
        $fail.Reason = "source database not found: $SourcePath"; return $fail
    }
    try { $b = [System.IO.File]::ReadAllBytes($SourcePath) }
    catch { $fail.Reason = "could not read $SourcePath"; return $fail }

    # Splice out every SHIM_REF naming the unwanted shim, one at a time, re-walking after
    # each so all offsets stay honest.
    $removed = 0
    while ($true) {
        $nodes = Get-SdbNodes -Bytes $b
        if (-not $nodes) { $fail.Reason = 'database did not parse'; return $fail }

        $stNode = $nodes | Where-Object { $_.Tag -eq 0x7801 } | Select-Object -First 1
        if (-not $stNode) { $fail.Reason = 'no string table'; return $fail }
        $stBase = $stNode.Pos + $stNode.HdrSize

        $target = $null
        foreach ($n in $nodes) {
            if ($n.Tag -ne 0x7009) { continue }                 # TAG_SHIM_REF
            foreach ($c in $nodes) {
                if ($c.Parent -ne $n.Index -or $c.Tag -ne 0x6001) { continue }
                $ref = [BitConverter]::ToUInt32($b, $c.Pos + 2)
                if ((Read-SdbRawString -Bytes $b -Base $stBase -Ref $ref) -eq $RemoveShim) { $target = $n }
            }
        }
        if (-not $target) { break }

        $cut = $target.Pos
        $len = $target.HdrSize + $target.DataSize

        $nb = New-Object byte[] ($b.Length - $len)
        [Array]::Copy($b, 0, $nb, 0, $cut)
        [Array]::Copy($b, $cut + $len, $nb, $cut, $b.Length - $cut - $len)

        # every enclosing list shrinks
        $p = $target.Parent
        while ($p -ge 0) {
            $anc = $nodes[$p]
            $sz = [BitConverter]::ToInt32($nb, $anc.Pos + 2)
            [BitConverter]::GetBytes([int]($sz - $len)).CopyTo($nb, $anc.Pos + 2)
            $p = $anc.Parent
        }

        # index entries are [8-byte key][DWORD TAGID]; shift any TAGID past the splice
        foreach ($n in $nodes) {
            if ($n.Tag -ne 0x9801 -or $n.Pos -ge $cut) { continue }   # TAG_INDEX_BITS
            $dataAt = $n.Pos + $n.HdrSize
            for ($i = 0; $i + 12 -le $n.DataSize; $i += 12) {
                $off = $dataAt + $i + 8
                $tid = [BitConverter]::ToUInt32($nb, $off)
                if ($tid -gt $cut) { [BitConverter]::GetBytes([uint32]($tid - $len)).CopyTo($nb, $off) }
            }
        }

        $b = $nb
        $removed++
        if ($removed -gt 32) { $fail.Reason = 'runaway removal'; return $fail }
    }

    if ($removed -eq 0) { $fail.Reason = "'$RemoveShim' is not applied by this database"; return $fail }

    $nodes = Get-SdbNodes -Bytes $b
    if (-not $nodes) { $fail.Reason = 'result did not parse'; return $fail }

    # Stamp our own database GUID so this is not confused with GOG's registration.
    $idNode = $nodes | Where-Object { $_.Tag -eq 0x9007 -and $_.DataSize -eq 16 } | Select-Object -First 1
    if ($idNode) { $NewGuid.ToByteArray().CopyTo($b, $idNode.Pos + $idNode.HdrSize) }

    # Rename it so the user sees whose database it is. Only safe when the string table is
    # last in the file: appending then moves nothing.
    $stNode = $nodes | Where-Object { $_.Tag -eq 0x7801 } | Select-Object -First 1
    $dbNode = $nodes | Where-Object { $_.Tag -eq 0x7001 } | Select-Object -First 1
    if ($stNode -and $dbNode -and $NewName) {
        $stEnd = $stNode.Pos + $stNode.HdrSize + $stNode.DataSize
        if ($stEnd -eq $b.Length) {
            $nameNode = $nodes | Where-Object { $_.Parent -eq $dbNode.Index -and $_.Tag -eq 0x6001 } | Select-Object -First 1
            if ($nameNode) {
                $strBytes = [System.Text.Encoding]::Unicode.GetBytes($NewName + "`0")
                $item = New-Object byte[] (6 + $strBytes.Length)
                [BitConverter]::GetBytes([uint16]0x8801).CopyTo($item, 0)
                [BitConverter]::GetBytes([int]$strBytes.Length).CopyTo($item, 2)
                $strBytes.CopyTo($item, 6)

                $newRef = ($b.Length + 6) - ($stNode.Pos + $stNode.HdrSize)

                $grown = New-Object byte[] ($b.Length + $item.Length)
                [Array]::Copy($b, 0, $grown, 0, $b.Length)
                [Array]::Copy($item, 0, $grown, $b.Length, $item.Length)

                [BitConverter]::GetBytes([int]($stNode.DataSize + $item.Length)).CopyTo($grown, $stNode.Pos + 2)
                [BitConverter]::GetBytes([uint32]$newRef).CopyTo($grown, $nameNode.Pos + 2)
                $b = $grown
            }
        }
    }

    try { [System.IO.File]::WriteAllBytes($DestPath, $b) }
    catch { $fail.Reason = "could not write $DestPath"; return $fail }

    # Read the result back through apphelp - the OS's parser, not the one that wrote it.
    $check = Get-SdbContents -Path $DestPath
    if (-not $check.Ok) { $fail.Reason = 'apphelp could not parse the result'; return $fail }
    if (@($check.Shims) -contains $RemoveShim) { $fail.Reason = "'$RemoveShim' survived removal"; return $fail }

    return [pscustomobject]@{ Ok = $true; Shims = @($check.Shims); Reason = '' }
}

function Read-SdbRawString {
    # Resolves a STRINGREF against the string table's data start.
    param([byte[]]$Bytes, [int]$Base, [uint32]$Ref)
    $p = $Base + [int]$Ref
    if ($p -lt 0 -or $p + 1 -ge $Bytes.Length) { return '' }
    $end = $p
    while ($end + 1 -lt $Bytes.Length -and -not ($Bytes[$end] -eq 0 -and $Bytes[$end + 1] -eq 0)) { $end += 2 }
    return [System.Text.Encoding]::Unicode.GetString($Bytes, $p, $end - $p)
}

function Get-UnaccountedShims {
    # Shims a database applies that this tool has no answer for. Removing the database
    # removes these too - only SingleProcAffinity is replaced, by the affinity launcher -
    # so they have to be surfaced rather than silently dropped.
    param([string[]]$Shims)
    return @($Shims | Where-Object { $_ -ne $script:BadShim -and $_ -ne $script:GoodShim })
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

        $contents = Get-SdbContents -Path $dbPath
        $shims    = @($contents.Shims)

        $unknown = @(Get-UnaccountedShims -Shims $shims)

        # Our own replacement database, so a re-run replaces it instead of refusing to
        # touch a database it does not recognise.
        $isOurs = ("{$guid}" -eq "{$($script:AffinityDbGuid)}") -or
                  ($contents.Name -eq $script:AffinityDbName)

        $found.Add([pscustomobject]@{
            Guid         = "{$guid}"
            Path         = $dbPath
            Description  = $dbDesc
            Readable     = $contents.Ok
            Entries      = $contents.Entries
            Shims        = $shims
            UnknownShims = $unknown
            IsOurs       = $isOurs
            HasBadShim   = ($contents.Ok -and ($shims -contains $script:BadShim))
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

function Get-StateValue {
    <#
        Reads one field out of the saved state, or '' if it is not there.

        state.json is data on disk, so it may be older, truncated or hand-edited, and this
        script runs under StrictMode 2.0 where reading a missing property is a terminating
        error rather than $null. A revert must not die because one field is absent - that
        is precisely when the state file matters most.
    #>
    param($State, [string]$Name)

    if (-not $State) { return '' }
    if ($State.PSObject.Properties.Name -notcontains $Name) { return '' }
    $value = $State.$Name
    if ($null -eq $value) { return '' }
    return $value
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

function Get-ProxyMarkerVersion {
    <#
        Returns the marker version embedded in a DLL, or $null if it is not one of ours.

        Matches the GUID-bearing PREFIX and parses the version that follows, rather than
        comparing the whole marker - see the note on $script:ProxyMarkerPrefix for why
        an exact comparison reintroduces the bug the marker was added to prevent.

        This answer is the authority for overwriting a file in the user's game folder
        WITHOUT backing it up, so it is deliberately more than a substring search:

          1. The file must be a 32-bit PE. That alone rejects logs, configs, saves, text
             files and 64-bit DLLs that merely happen to contain the bytes.
          2. The marker is emitted as a NUL-terminated C string literal, so the version
             digits must be followed by a NUL. A chance occurrence of the prefix inside
             some other blob almost certainly is not.

        It is still evidence rather than proof - a string in a binary always is - but the
        GUID makes accidental collision implausible and the structure makes a partial
        match fail closed. When in doubt this returns $null, and the caller then treats
        the file as third-party: backed up and chained, never silently replaced.
    #>
    param([string]$Path)

    # Guard an empty prefix: every string contains "", which would classify a third-party
    # DLL as ours and overwrite it without a backup.
    if ([string]::IsNullOrEmpty($script:ProxyMarkerPrefix)) {
        throw 'Internal error: proxy marker prefix is not set. Refusing to classify DLLs.'
    }

    if (-not (Test-Pe32 -Path $Path)) { return $null }

    try { $bytes = [System.IO.File]::ReadAllBytes($Path) }
    catch { return $null }

    $text = [System.Text.Encoding]::ASCII.GetString($bytes)

    foreach ($legacy in $script:LegacyProxyMarkers) {
        if ($text.Contains($legacy)) { return 1 }
    }

    $at = $text.IndexOf($script:ProxyMarkerPrefix, [System.StringComparison]::Ordinal)
    if ($at -lt 0) { return $null }

    # Read the version digits straight out of the byte array, not the ASCII projection.
    $p = $at + $script:ProxyMarkerPrefix.Length
    $digits = ''
    while ($p -lt $bytes.Length -and $bytes[$p] -ge 0x30 -and $bytes[$p] -le 0x39) {
        $digits += [char]$bytes[$p]
        $p++
    }
    if ($digits -eq '') { return $null }
    if ($p -ge $bytes.Length -or $bytes[$p] -ne 0) { return $null }   # not NUL-terminated

    return [int]$digits
}

function Test-IsOurProxy {
    # Our proxy DLLs embed a marker string so the installer can recognise any build of
    # itself. Matching on file hash would break the moment the proxy is rebuilt.
    param([string]$Path)
    return ($null -ne (Get-ProxyMarkerVersion -Path $Path))
}

function Test-WasInstalledByUs {
    <#
        Covers proxies installed before any marker existed. Those DLLs carry nothing to
        match on, so without this an upgrade would classify our own previous build as
        third-party and chain the new proxy to it. Marked builds are handled by
        Get-ProxyMarkerVersion; this is the fallback for the unmarked ones.
    #>
    param([string]$Path)

    $s = Get-State
    if (-not $s) { return $false }
    $names = $s.PSObject.Properties.Name
    foreach ($k in @('ProxyTarget', 'PadTarget')) {
        if ($names -contains $k) {
            $v = [string]$s.$k
            if ($v -and ($v -eq $Path)) { return $true }
        }
    }
    return $false
}

function Install-ProxyDll {
    <#
        Shared installer for the in-repo proxy DLLs (d3d9.dll and dinput8.dll).

        Only one file can carry each of those names. If the game folder already has one
        from another project (dgVoodoo, DXVK, ReShade), it is renamed to <name>_chain.dll
        and our proxy forwards to it rather than replacing it.
    #>
    param(
        [string]   $Folder,
        [string]   $Source,
        [string]   $TargetName,
        [string]   $ChainName,
        [string]   $IniName,
        [string[]] $IniLines,
        [string]   $Label
    )

    if (-not (Test-Path -LiteralPath $Source)) {
        Write-Warn2 "$Label DLL not found: $Source"
        Write-Info  'Build it first:   src\build.cmd'
        return $null
    }
    if (-not (Test-Pe32 -Path $Source)) {
        throw "$Source is not a 32-bit DLL. BGE.exe is a 32-bit process and cannot load it. Rebuild with src\build.cmd."
    }

    $target      = Join-Path $Folder $TargetName
    $chainTarget = Join-Path $Folder $ChainName

    if (Test-Path -LiteralPath $target) {
        # Identify our own DLL by its embedded marker, not by hashing against the source.
        # A rebuilt proxy has different bytes, and a hash check would classify the
        # already-installed copy as third-party and chain the new build to the old one.
        $installedVersion = Get-ProxyMarkerVersion -Path $target
        $isOurs = ($null -ne $installedVersion) -or (Test-WasInstalledByUs -Path $target)

        if ($isOurs) {
            # Any version of our own proxy is replaced in place, never chained to.
            if ($null -ne $installedVersion -and $installedVersion -gt $script:ProxyMarkerVersion) {
                Write-Warn2 ("$TargetName in the game folder is a NEWER build of this proxy " +
                             "(marker v$installedVersion, this installer ships v$($script:ProxyMarkerVersion)). " +
                             'Replacing it with the bundled copy - re-run the newer installer to undo.')
            }
        }
        else {
            if (Test-Path -LiteralPath $chainTarget) {
                throw ("Both $TargetName and $ChainName already exist in the game folder. " +
                       "Resolve that by hand so nothing is lost, then re-run.")
            }
            Backup-File -Path $target -Label "$TargetName-preexisting" | Out-Null
            Move-Item -LiteralPath $target -Destination $chainTarget -Force
            Write-Ok "Existing $TargetName renamed to $ChainName; the proxy will forward to it"
        }
    }

    Copy-Item -LiteralPath $Source -Destination $target -Force

    # Report the chain from what is on disk rather than from what this run happened to do,
    # so a second run still records it and -Revert can put it back.
    $chained = ''
    if (Test-Path -LiteralPath $chainTarget) { $chained = $chainTarget }

    $ini = Join-Path $Folder $IniName
    if ((Test-Path -LiteralPath $ini) -and -not $ResetConfig) {
        Write-Info "$IniName exists - keeping your settings (use -ResetConfig to regenerate)"
    }
    else {
        [System.IO.File]::WriteAllLines($ini, $IniLines, (New-Object System.Text.UTF8Encoding($false)))
        Write-Info "config      -> $ini"
    }

    Write-Info "$TargetName -> $target"

    return [pscustomobject]@{ Target = $target; Ini = $ini; Chained = $chained }
}

function Install-WindowedProxy {
    # Forces the game out of exclusive fullscreen so Alt+Tab is safe.
    param([string]$Folder)

    $src = $ProxyPath
    if (-not $src) { $src = Join-Path $PSScriptRoot 'dist\d3d9.dll' }

    $desc = @('windowed with a title bar', 'borderless, centred at game resolution',
              'borderless, stretched to fill the monitor')[$WindowMode]

    $r = Install-ProxyDll -Folder $Folder -Source $src -TargetName 'd3d9.dll' `
            -ChainName 'd3d9_chain.dll' -IniName 'd3d9_windowed.ini' -Label 'Windowed proxy' `
            -IniLines @(
                '; d3d9_windowed - forces the game out of exclusive fullscreen so Alt+Tab is safe.',
                '; Mode 0 = windowed   1 = borderless centred (default)   2 = borderless stretched',
                ';',
                '; Fill16BitModeList=1 stands in a 16-bit display-mode list for GOG''s launcher and',
                '; settings app when the driver reports none. Without it, on hardware that does not',
                '; report 16-bit modes, the settings app shows empty Resolution and Refresh rate',
                '; dropdowns and the launcher re-runs it instead of starting the game. 0 passes the',
                '; driver''s answer through untouched.',
                '[Display]',
                "Mode=$WindowMode",
                'Fill16BitModeList=1',
                '',
                # Log and Chain live in [General] in every proxy ini this installer writes,
                # so one file teaches you where to find them in the other.
                '[General]',
                'Log=0',
                'Chain=d3d9_chain.dll'
            )
    if ($r) { Write-Ok "Windowed proxy installed (mode $WindowMode - $desc)" }
    return $r
}

function Install-ControllerSupport {
    <#
        Installs the dinput8 proxy that maps an XInput pad onto the keyboard and mouse
        state the game already reads.

        BGE.exe has no gamepad code: it imports only DirectInput8Create and contains no
        joystick vocabulary. Its bindings are stored as DirectInput scan codes, so the
        proxy writes controller state straight into the 256-byte keyboard array and the
        mouse state the game polls. Defaults below mirror the game's own bindings.
    #>
    param([string]$Folder)

    $src = $PadPath
    if (-not $src) { $src = Join-Path $PSScriptRoot 'dist\dinput8.dll' }

    $r = Install-ProxyDll -Folder $Folder -Source $src -TargetName 'dinput8.dll' `
            -ChainName 'dinput8_chain.dll' -IniName 'dinput8_xinput.ini' -Label 'Controller' `
            -IniLines @(
                '; dinput8_xinput - XInput (Xbox) controller support for Beyond Good & Evil.',
                '; Values are DirectInput scan codes (hex) or MOUSE1..MOUSE8. 0 = unmapped.',
                '[Buttons]',
                'A=MOUSE1                ; primary action',
                'B=MOUSE2                ; secondary action',
                'X=0x10                  ; Q - use object',
                'Y=0x12                  ; E - buddy / compass',
                'LB=0x2A                 ; LShift - photo/camera mode (registry calls it "Look mode")',
                'RB=0x2E                 ; C - center view',
                'LT=0x1D                 ; LCtrl - crouch',
                'RT=0x39                 ; Space - run / accelerate',
                'Start=0x01              ; Esc - menu',
                'Back=0x0F               ; Tab - map',
                'LS=0',
                'RS=0x2E                 ; C - center view',
                'DPadUp=0',
                'DPadDown=0',
                'DPadLeft=0x03           ; 2 - inventory prev',
                'DPadRight=0x04          ; 3 - inventory next',
                'TriggerThreshold=30      ; XINPUT_GAMEPAD_TRIGGER_THRESHOLD',
                '',
                '[Sticks]',
                'LeftUp=0x11             ; W',
                'LeftDown=0x1F           ; S',
                'LeftLeft=0x1E           ; A',
                'LeftRight=0x20          ; D',
                "Deadzone=$PadDeadzone         ; radial, in raw XInput units",
                "LookDeadzone=$PadLookDeadzone",
                "LookSpeed=$PadLookSpeed          ; mouse counts per second at full deflection",
                "InvertLook=$([int]$PadInvertLook.IsPresent)",
                '',
                '[General]',
                'Log=0',
                'Chain=dinput8_chain.dll'
            )
    if ($r) {
        Write-Ok "Controller support installed (look $PadLookSpeed counts/sec, deadzone $PadDeadzone/$PadLookDeadzone)"
        Write-Info 'Left stick moves, right stick looks, A = primary action. Remap in dinput8_xinput.ini.'
    }
    return $r
}

#endregion

#region ---------------------------------------------------------------- modes

function Show-SdbInventory {
    # Everything a database actually does, read out of the file rather than assumed.
    param($Db)

    Write-Info "GUID  : $($Db.Guid)"
    Write-Info "Desc  : $($Db.Description)"
    Write-Info "Path  : $($Db.Path)"

    if (-not $Db.Readable) {
        Write-Warn2 'Contents COULD NOT BE READ - this is "unknown", not "empty"'
        return
    }
    if (@($Db.Entries).Count -eq 0) {
        Write-Info 'Contents: database parsed but patches no executables'
        return
    }

    foreach ($e in $Db.Entries) {
        if (@($e.Mods).Count -eq 0) {
            Write-Info "$($e.Exe) : (no modifications)"
            continue
        }
        foreach ($m in $e.Mods) {
            if     ($m.Name -eq $script:BadShim)  { $tail = '  <- the bug; removed by this tool' }
            elseif ($m.Name -eq $script:GoodShim) { $tail = '  <- kept' }
            else                                  { $tail = '  <- NOT accounted for by this tool' }
            Write-Info ("{0} : {1}{2}" -f $e.Exe, $m.Name, $tail)
        }
    }
    if (@($Db.UnknownShims).Count -gt 0) {
        Write-Warn2 ("This database also applies: $($Db.UnknownShims -join ', '). " +
                     'Removing it removes those too.')
    }
}

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
    if ($db -and $db.IsOurs) {
        Write-Ok 'Replacement shim database installed'
        Write-Info "Alt+Tab is free, and '$($script:GoodShim)' still applies to BGE.exe however it is launched."
        Show-SdbInventory -Db $db
    }
    elseif ($db) {
        Write-Bad 'Shim database INSTALLED - Alt+Tab is blocked'
        Show-SdbInventory -Db $db
    }
    else {
        Write-Warn2 'No BGE.exe shim database installed'
        Write-Info  "Alt+Tab is not blocked, but '$($script:GoodShim)' is not applied either -"
        Write-Info  '  the game is not pinned to one core. Re-run to install the replacement.'
    }

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

    if (Test-Path -LiteralPath (Join-Path $Folder 'dinput8_xinput.ini')) {
        Write-Ok 'Controller support installed (XInput -> DirectInput keyboard/mouse)'
        if (Test-Path -LiteralPath (Join-Path $Folder 'dinput8_chain.dll')) {
            Write-Info 'Chaining to dinput8_chain.dll'
        }
    }
    else {
        Write-Warn2 'Controller support not installed'
    }

    $applied = Get-StateValue (Get-State) 'AppliedUtc'
    if ($applied) { Write-Info "Fix applied on: $applied UTC" }
    Write-Host ''
}

function Invoke-Apply {
    param([string]$Folder, [string[]]$Components)

    Write-Head 'Plan'

    $doAltTab     = $Components -contains 'AltTab'
    $doWindowed   = $Components -contains 'Windowed'
    $doController = $Components -contains 'Controller'

    $db = $null
    if ($doAltTab) {
        $db = Get-BgeShimDatabase
        if ($db -and $db.IsOurs) {
            Write-Ok "Our affinity-only database is already installed - it will be reinstalled"
        }
        elseif ($db -and -not $db.Readable) {
            throw ("A shim database is registered for BGE.exe but its contents could not be read " +
                   "(apphelp.dll could not parse it). Refusing to remove a database whose effects " +
                   "are unknown. Inspect: $($db.Path)")
        }
        elseif ($db -and -not $db.HasBadShim) {
            throw ("A shim database is registered for BGE.exe but does not apply '$($script:BadShim)'. " +
                   "It applies: $(@($db.Shims) -join ', '). Refusing to remove a database this script " +
                   "did not identify. Inspect: $($db.Path)")
        }
    }

    $steps = New-Object System.Collections.Generic.List[string]
    if ($doAltTab) {
        if ($db) { $steps.Add("Remove shim database $($db.Guid) ('$($db.Description)')") }
        else     { $steps.Add('Shim database already absent - nothing to remove') }

        # Removing the database removes everything in it, so anything beyond the two shims
        # this tool understands has to be surfaced before the confirmation prompt, not after.
        if ($db -and @($db.UnknownShims).Count -gt 0) {
            Write-Warn2 ("The database also applies shims this tool does not account for: " +
                         "$($db.UnknownShims -join ', ').")
            Write-Warn2 ("Removing it drops those too. Only '$($script:GoodShim)' is carried over " +
                         '(into the replacement database); the rest are simply lost.')
            $steps.Add("  ...which also removes: $($db.UnknownShims -join ', ')")
        }
        $steps.Add("Install replacement database applying only '$($script:GoodShim)'")
    }
    if ($doWindowed)   { $steps.Add("Install d3d9 windowed proxy (window mode $WindowMode)") }
    if ($doController) { $steps.Add('Install dinput8 XInput controller support') }

    if ($steps.Count -eq 0) { $steps.Add('Nothing selected') }

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
        AffinityDbGuid = ''
        AffinityDbPath = ''
        ProxyTarget    = ''
        ProxyIni       = ''
        ProxyChained   = ''
        PadTarget      = ''
        PadIni         = ''
        PadChained     = ''
    }

    # Carry forward what an earlier run recorded. Installing one component must not erase
    # another's revert data - without this, `-Component Controller` on an existing install
    # would leave -Revert unable to put the d3d9 proxy back.
    $prev = Get-State
    if ($prev) {
        foreach ($key in @($state.Keys)) {
            if ($key -in @('AppliedUtc', 'GamePath')) { continue }
            if (($prev.PSObject.Properties.Name -contains $key) -and $prev.$key) {
                $state[$key] = $prev.$key
            }
        }
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

    # ---- 1b. replacement database: SingleProcAffinity, machine-wide -------
    # The point of this step is that affinity stays a property of BGE.exe itself, applied
    # by the OS however the game is launched - GOG Galaxy, Steam shortcut, the .exe
    # directly. A desktop shortcut only pins the runs that go through it.
    $affinityInstalled = $false
    if ($doAltTab -and $PSCmdlet.ShouldProcess("SingleProcAffinity-only database", 'Build and install')) {
        $src = ''
        foreach ($candidate in @(
            $state.ShimSource,
            (Join-Path $Folder 'goggame.sdb'),
            (Join-Path $script:BackupDir 'goggame-shipped.sdb'),
            (Join-Path $script:BackupDir 'goggame-installed.sdb')
        )) {
            if ($candidate -and (Test-Path -LiteralPath $candidate)) { $src = $candidate; break }
        }

        if (-not $src) {
            Write-Warn2 "No source database found to derive '$($script:GoodShim)' from."
        }
        else {
            if (-not (Test-Path -LiteralPath $script:StateDir)) {
                New-Item -ItemType Directory -Path $script:StateDir -Force | Out-Null
            }
            $derived = Join-Path $script:StateDir $script:AffinityDbFile

            Write-Step "Deriving affinity-only database from $src"
            $made = New-AffinityOnlySdb -SourcePath $src -DestPath $derived `
                        -RemoveShim $script:BadShim -NewGuid $script:AffinityDbGuid `
                        -NewName $script:AffinityDbName

            if (-not $made.Ok) {
                Write-Warn2 "Could not build the replacement database: $($made.Reason)"
            }
            else {
                Write-Step "Installing $derived"
                Invoke-SdbInst -Arguments @('-q', $derived) | Out-Null

                # Verify rather than trust sdbinst's exit code: re-read what is actually
                # registered for BGE.exe now and confirm it is ours, applies the affinity
                # shim, and does not apply the one we just removed.
                $now = Get-BgeShimDatabase
                if ($now -and $now.Readable -and $now.IsOurs -and
                    (@($now.Shims) -contains $script:GoodShim) -and
                    -not (@($now.Shims) -contains $script:BadShim)) {
                    $affinityInstalled = $true
                    $state.AffinityDbGuid = $now.Guid
                    $state.AffinityDbPath = $derived
                    Write-Ok "'$($script:GoodShim)' reinstalled machine-wide - affinity applies however the game is launched"
                }
                else {
                    Write-Warn2 'The replacement database did not register correctly; rolling it back.'
                    Invoke-SdbInst -Arguments @('-q', '-g', "{$($script:AffinityDbGuid)}") | Out-Null
                }
            }
        }

        # No fallback. A desktop shortcut only pins the runs that go through it, so
        # offering one here would report success while quietly delivering something
        # weaker than what GOG shipped. Say what is true instead.
        if (-not $affinityInstalled) {
            Write-Warn2 "'$($script:GoodShim)' is NOT applied. GOG's database was removed and the"
            Write-Warn2 '  replacement could not be installed, so the game is no longer pinned to one core.'
            Write-Info  '  Expect timing or audio problems. Re-run, or use -Revert to put GOG''s database back.'
        }
    }

    # ---- 2. windowed proxy ----------------------------------------------
    if ($doWindowed) {
        if ($PSCmdlet.ShouldProcess($Folder, 'Install d3d9 windowed proxy')) {
            $px = Install-WindowedProxy -Folder $Folder
            if ($px) {
                $state.ProxyTarget  = $px.Target
                $state.ProxyIni     = $px.Ini
                $state.ProxyChained = $px.Chained
            }
        }
    }
    elseif ($doAltTab) {
        Write-Warn2 'Proxy skipped - the game keeps an exclusive fullscreen device.'
        Write-Info  'Alt+Tab will work, but the HUD can corrupt when you return.'
    }

    # ---- 4. controller support -------------------------------------------
    if ($doController) {
        if ($PSCmdlet.ShouldProcess($Folder, 'Install XInput controller support')) {
            $pad = Install-ControllerSupport -Folder $Folder
            if ($pad) {
                $state.PadTarget  = $pad.Target
                $state.PadIni     = $pad.Ini
                $state.PadChained = $pad.Chained
            }
        }
    }

    if (-not $WhatIfPreference) {
        Save-State -State $state
        Write-Host ''
        Write-Ok 'Done.'
        Write-Info "Undo with: .\Fix-BGE.ps1 -Revert"
        Write-Host ''
    }
}

function Invoke-Revert {
    param([string[]]$Components)

    Write-Head 'Reverting'

    $doAltTab     = $Components -contains 'AltTab'
    $doWindowed   = $Components -contains 'Windowed'
    $doController = $Components -contains 'Controller'
    $partial      = @($Components).Count -lt 3

    if ($partial) { Write-Info "Reverting only: $($Components -join ', ')" }

    $state = Get-State
    if (-not $state) {
        Write-Warn2 'No saved state found. Attempting a best-effort revert.'
    }

    # ---- our replacement database ---------------------------------------
    # Uninstall ours first, otherwise reinstalling GOG's would leave two databases
    # registered for BGE.exe.
    $ourGuid = "{$($script:AffinityDbGuid)}"
    $recordedGuid = Get-StateValue $state 'AffinityDbGuid'
    if ($recordedGuid) { $ourGuid = $recordedGuid }
    $current = Get-BgeShimDatabase
    if ($doAltTab -and $current -and $current.IsOurs) {
        if ($PSCmdlet.ShouldProcess($ourGuid, 'Uninstall affinity-only database')) {
            Write-Step "Uninstalling affinity-only database $ourGuid"
            Invoke-SdbInst -Arguments @('-q', '-g', $ourGuid) | Out-Null
            Write-Ok 'Affinity-only database removed'
        }
    }

    # ---- shim database ---------------------------------------------------
    if ($doAltTab -and -not (Get-BgeShimDatabase)) {
        $source = ''
        foreach ($candidate in @(
            (Join-Path $script:BackupDir 'goggame-installed.sdb'),
            (Join-Path $script:BackupDir 'goggame-shipped.sdb')
        )) {
            if (Test-Path -LiteralPath $candidate) { $source = $candidate; break }
        }

        $recordedGame = Get-StateValue $state 'GamePath'
        if (-not $source -and $recordedGame) {
            $shipped = Join-Path $recordedGame 'goggame.sdb'
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
    elseif ($doAltTab) {
        Write-Ok 'Shim database already installed - nothing to restore'
    }

    # ---- proxy DLLs ------------------------------------------------------
    # If a proxy displaced an existing DLL it was renamed to <name>_chain.dll;
    # putting it back is part of leaving the folder as we found it.
    if ($state) {
        $sets = @()
        if ($doWindowed)   { $sets += @{ T = 'ProxyTarget'; I = 'ProxyIni'; C = 'ProxyChained'; Label = 'Windowed proxy'; Keys = @('ProxyTarget','ProxyIni','ProxyChained') } }
        if ($doController) { $sets += @{ T = 'PadTarget';   I = 'PadIni';   C = 'PadChained';   Label = 'Controller support'; Keys = @('PadTarget','PadIni','PadChained') } }
        $names = $state.PSObject.Properties.Name

        foreach ($set in $sets) {
            $t = ''; $i = ''; $c = ''
            if ($names -contains $set.T) { $t = [string]$state.($set.T) }
            if ($names -contains $set.I) { $i = [string]$state.($set.I) }
            if ($names -contains $set.C) { $c = [string]$state.($set.C) }

            if ($t -and (Test-Path -LiteralPath $t)) {
                if ($PSCmdlet.ShouldProcess($t, "Remove $($set.Label)")) {
                    Remove-Item -LiteralPath $t -Force
                    if ($i -and (Test-Path -LiteralPath $i)) { Remove-Item -LiteralPath $i -Force }
                    Write-Ok "$($set.Label) removed"

                    if ($c -and (Test-Path -LiteralPath $c)) {
                        Move-Item -LiteralPath $c -Destination $t -Force
                        Write-Ok "Restored the previous $(Split-Path $t -Leaf) from $(Split-Path $c -Leaf)"
                    }
                }
            }
        }
    }

    if (-not $WhatIfPreference) {
        if ($partial -and $state) {
            # A partial revert must not discard the state file: whatever is still
            # installed still needs its revert data. Clear only what was undone.
            $keep = @{}
            foreach ($p in $state.PSObject.Properties) { $keep[$p.Name] = $p.Value }

            $clear = @()
            if ($doAltTab)     { $clear += @('ShimGuid', 'ShimBackup', 'ShimSource', 'AffinityDbGuid', 'AffinityDbPath') }
            if ($doWindowed)   { $clear += @('ProxyTarget', 'ProxyIni', 'ProxyChained') }
            if ($doController) { $clear += @('PadTarget', 'PadIni', 'PadChained') }
            foreach ($k in $clear) { if ($keep.ContainsKey($k)) { $keep[$k] = '' } }

            Save-State -State $keep
        }
        elseif (Test-Path -LiteralPath $script:StateFile) {
            Remove-Item -LiteralPath $script:StateFile -Force
        }
    }

    Write-Host ''
    if ($partial) { Write-Ok "Reverted: $($Components -join ', ')" }
    else          { Write-Ok 'Revert complete.' }
    Write-Host ''
}

#endregion

#region ---------------------------------------------------------------- entry point

try {
    Write-Host ''
    Write-Host "  $script:AppName" -ForegroundColor White
    Write-Host '  Fixes for the GOG release of Beyond Good & Evil (2003)' -ForegroundColor DarkGray

    $folder = Resolve-GameFolder -Explicit $GamePath

    # -Status and -Revert are separate parameter sets, so they can no longer be combined;
    # this is a plain dispatch, not a precedence rule that silently drops one of them.
    if ($PSCmdlet.ParameterSetName -eq 'Status') {
        Show-Status -Folder $folder
        return
    }

    $components = Resolve-Components -Requested $Component -ForRevert:$Revert
    if ($PSCmdlet.ParameterSetName -ne 'Revert') {
        Assert-ComponentOptions -Components $components -Bound $PSBoundParameters
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
            # $PSBoundParameters must be read here, in script scope, not inside the function.
            # The desktop is resolved here too, while we are still the invoking user.
            exit (Invoke-SelfElevate -Bound $PSBoundParameters)
        }
    }

    $running = @(Get-Process -Name 'BGE', 'CheckApplication' -ErrorAction SilentlyContinue)
    if ($running.Count -gt 0) {
        Write-Warn2 'Beyond Good & Evil appears to be running. Close it before continuing.'
    }

    if ($Revert) { Invoke-Revert -Components $components }
    else         { Invoke-Apply -Folder $folder -Components $components }
}
catch {
    Write-Host ''
    Write-Bad $_.Exception.Message
    Write-Host ''
    exit 1
}
finally {
    # Replaces -NoExit on the elevated relaunch. Only an interactive elevated child pauses,
    # so -Force still runs to completion and closes on its own - which is the whole point
    # of -Force. Runs on the error path too, so a failure is readable before the window goes.
    if ($ElevatedRelaunch -and -not $Force -and -not $WhatIfPreference) {
        try {
            Write-Host ''
            Read-Host '  Press Enter to close this window'
        }
        catch { }
    }
}

#endregion
