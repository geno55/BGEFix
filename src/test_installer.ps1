<#
    test_installer - checks how the installer classifies a DLL already in the game folder.

    That decision decides whether a file is REPLACED or renamed to *_chain.dll and backed
    up, so getting it wrong either destroys somebody's third-party wrapper or chains our
    new proxy to our own old one. Neither shows up until a user hits it.

    The interesting case cannot be exercised by shipping code alone, because it only
    happens on the NEXT release: an installer that matched its own marker exactly would
    meet the previous version's DLL and classify it as third-party. So the fixtures here
    synthesise markers this build does not carry - an older one and a newer one - and
    assert both are still recognised.

    The functions under test are lifted out of Fix-BGE.ps1 by the PowerShell parser,
    so nothing is reimplemented and the test cannot drift from the installer.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

# Match the installer's own strictness. Without this the harness silently tolerates
# reading an unset variable or a missing property, while the real script - which sets
# StrictMode 2.0 - dies on it. That gap shipped a crash in Initialize-SdbApi: the tests
# exercised the function happily and the first real apply failed on its first line.
Set-StrictMode -Version 2.0

$repo = Split-Path -Parent $PSScriptRoot
$src  = Join-Path $repo 'Fix-BGE.ps1'
$tmp  = Join-Path ([System.IO.Path]::GetTempPath()) ("bgefix-installer-test-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tmp | Out-Null

# --- lift the functions and marker constants out of the installer -------------------
$ast  = [System.Management.Automation.Language.Parser]::ParseFile($src, [ref]$null, [ref]$null)
$want = 'ConvertTo-ProcessArgument', 'Invoke-SelfElevate',
        'Resolve-Components', 'Assert-ComponentOptions', 'Resolve-ComponentSpec',
        'Test-CanPrompt', 'Get-ComponentInstallState', 'Show-ComponentSelector',
        'Test-Pe32', 'Get-ProxyMarkerVersion', 'Test-IsOurProxy',
        'Initialize-SdbApi', 'Get-SdbTagString', 'Get-SdbChildString', 'Get-SdbContents',
        'Get-SdbNodes', 'New-AffinityOnlySdb', 'Read-SdbRawString',
        'Get-UnaccountedShims', 'Show-Status', 'Show-SdbInventory',
        'Get-StateValue',
        'Write-Head', 'Write-Step', 'Write-Ok', 'Write-Warn2', 'Write-Bad', 'Write-Info'
$fns  = $ast.FindAll({
    param($n)
    $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $want -contains $n.Name
}, $true)
if ($fns.Count -ne $want.Count) {
    throw "expected to extract $($want.Count) functions from Fix-BGE.ps1, got $($fns.Count)"
}
foreach ($f in $fns) { . ([scriptblock]::Create($f.Extent.Text)) }

# Every top-level $script: assignment, not a hand-picked list. A list drifts: it silently
# omitted the SdbApiReady/SdbApiTried declarations, so the harness saw a different
# initial state than the real script and could not reproduce a StrictMode failure in it.
foreach ($line in (Select-String -Path $src -Pattern '^\$script:\w+\s*=')) {
    . ([scriptblock]::Create($line.Line))
}

$script:fail = 0
function Check {
    param([string]$What, [bool]$Ok, [string]$Detail = '')
    Write-Host ("  {0,-4} {1}{2}" -f $(if ($Ok) { 'PASS' } else { 'FAIL' }), $What,
                $(if ($Detail) { " -> $Detail" } else { '' }))
    if (-not $Ok) { $script:fail++ }
}

# A file that satisfies Test-Pe32: MZ, e_lfanew at 0x3C, PE\0\0, machine 0x14C.
# Synthesised rather than borrowed from Windows so the test is hermetic.
function New-Pe32Stub {
    param([string]$Path, [byte[]]$Append = @())
    $b = New-Object byte[] 256
    $b[0] = 0x4D; $b[1] = 0x5A                                   # MZ
    [BitConverter]::GetBytes([int]0x80).CopyTo($b, 0x3C)         # e_lfanew
    $b[0x80] = 0x50; $b[0x81] = 0x45                             # PE
    [BitConverter]::GetBytes([uint16]0x14C).CopyTo($b, 0x84)     # IMAGE_FILE_MACHINE_I386
    [System.IO.File]::WriteAllBytes($Path, ($b + $Append))
}

function Marker { param([string]$Text) return ([System.Text.Encoding]::ASCII.GetBytes($Text) + [byte]0) }

function New-TestSdb {
    <#
        Writes a real, apphelp-parseable shim database applying an arbitrary set of shims
        to an executable. The point is to build databases this tool has never seen - with
        shim names it does not hardcode - because that is the case the old byte-scanning
        implementation could not represent at all.

        Format: 12-byte header, then a tree of tags. The tag type is the high nibble of
        the tag word; 0x7000 is a list with a DWORD size. A STRINGREF (0x6000) holds the
        offset of the string DATA relative to the string table's data start - verified
        against a real GOG goggame.sdb, not assumed.
    #>
    param([string]$Path, [string]$DbName, [string]$Exe, [string[]]$Shims)

    $ms = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter($ms)
    $bw.Write([int]2); $bw.Write([int]1)
    $bw.Write([byte[]][System.Text.Encoding]::ASCII.GetBytes('sdbf'))

    $fixups = @{}
    $wref = {
        param($s)
        $bw.Write([uint16]0x6001)
        $fixups[[int]$ms.Position] = $s
        $bw.Write([uint32]0)
    }

    $bw.Write([uint16]0x7001)                                   # TAG_DATABASE
    $dbSizePos = [int]$ms.Position; $bw.Write([uint32]0)
    $dbStart   = [int]$ms.Position
    & $wref $DbName

    # TAG_DATABASE_ID, so the fixture exercises GUID stamping the way a real one does.
    $bw.Write([uint16]0x9007)
    $bw.Write([uint32]16)
    $bw.Write(([guid]'11111111-2222-3333-4444-555555555555').ToByteArray())

    $bw.Write([uint16]0x7007)                                   # TAG_EXE
    $exeSizePos = [int]$ms.Position; $bw.Write([uint32]0)
    $exeStart   = [int]$ms.Position
    & $wref $Exe

    $bw.Write([uint16]0x7008)                                   # TAG_MATCHING_FILE
    $mfPos = [int]$ms.Position; $bw.Write([uint32]0)
    $mfStart = [int]$ms.Position
    & $wref '*'
    $mfEnd = [int]$ms.Position

    foreach ($shim in $Shims) {
        $bw.Write([uint16]0x7009)                               # TAG_SHIM_REF
        $p = [int]$ms.Position; $bw.Write([uint32]0)
        $s = [int]$ms.Position
        & $wref $shim
        $e = [int]$ms.Position
        $ms.Position = $p; $bw.Write([uint32]($e - $s)); $ms.Position = $e
    }
    $exeEnd = [int]$ms.Position
    $dbEnd  = [int]$ms.Position

    $bw.Write([uint16]0x7801)                                   # TAG_STRINGTABLE
    $stSizePos = [int]$ms.Position; $bw.Write([uint32]0)
    $stStart   = [int]$ms.Position

    $offsets = @{}
    foreach ($s in (@($DbName, $Exe, '*') + $Shims | Select-Object -Unique)) {
        $bw.Write([uint16]0x8801)                               # TAG_STRINGTABLE_ITEM
        $bytes = [System.Text.Encoding]::Unicode.GetBytes($s + "`0")
        $bw.Write([uint32]$bytes.Length)
        $offsets[$s] = [int]$ms.Position - $stStart
        $bw.Write($bytes)
    }
    $stEnd = [int]$ms.Position

    $ms.Position = $mfPos;      $bw.Write([uint32]($mfEnd  - $mfStart))
    $ms.Position = $exeSizePos; $bw.Write([uint32]($exeEnd - $exeStart))
    $ms.Position = $dbSizePos;  $bw.Write([uint32]($dbEnd  - $dbStart))
    $ms.Position = $stSizePos;  $bw.Write([uint32]($stEnd  - $stStart))
    foreach ($kv in $fixups.GetEnumerator()) {
        $ms.Position = $kv.Key
        $bw.Write([uint32]$offsets[$kv.Value])
    }
    $bw.Flush()
    [System.IO.File]::WriteAllBytes($Path, $ms.ToArray())
    $bw.Dispose()
}

try {
    Write-Host ''
    Write-Host '=== installer proxy classification ==='
    Write-Host ''
    Write-Host ("  prefix  : {0}" -f $script:ProxyMarkerPrefix)
    Write-Host ("  shipping: v{0}" -f $script:ProxyMarkerVersion)
    Write-Host ("  legacy  : {0}" -f ($script:LegacyProxyMarkers -join ', '))
    Write-Host ''

    # ---------------------------------------------------------------- fixtures
    $older     = Join-Path $tmp 'older.dll'      # a previous release's marker
    $newer     = Join-Path $tmp 'newer.dll'      # a marker from a future release
    $plainPe   = Join-Path $tmp 'thirdparty.dll' # someone else's 32-bit wrapper
    $textFile  = Join-Path $tmp 'notes.txt'      # merely contains the bytes
    $unterm    = Join-Path $tmp 'unterm.dll'     # prefix present but not NUL-terminated
    $truncated = Join-Path $tmp 'truncated.dll'  # not a PE at all

    New-Pe32Stub -Path $older -Append (Marker $script:LegacyProxyMarkers[0])
    New-Pe32Stub -Path $newer -Append (Marker "$($script:ProxyMarkerPrefix)99")
    New-Pe32Stub -Path $plainPe
    New-Pe32Stub -Path $unterm -Append ([System.Text.Encoding]::ASCII.GetBytes("$($script:ProxyMarkerPrefix)99 and then more text"))
    Set-Content -Path $textFile -Value "$($script:ProxyMarkerPrefix)2 turned up in a log line" -Encoding ascii
    [System.IO.File]::WriteAllBytes($truncated, [byte[]](1, 2, 3, 4))

    # ---------------------------------------------------------------- the version trap
    Write-Host '  -- the marker version is parsed, never compared --'

    $bundled = Join-Path $repo 'dist\d3d9.dll'
    if (Test-Path -LiteralPath $bundled) {
        $v = Get-ProxyMarkerVersion -Path $bundled
        Check 'the bundled proxy is recognised' ($v -eq $script:ProxyMarkerVersion) "v$v"
    }
    else {
        Write-Host '  SKIP dist\d3d9.dll not built'
    }

    $vOld = Get-ProxyMarkerVersion -Path $older
    Check 'a PREVIOUS release is still recognised as ours' ($null -ne $vOld) "v$vOld"

    $vNew = Get-ProxyMarkerVersion -Path $newer
    Check 'a FUTURE release is still recognised as ours' ($vNew -eq 99) "v$vNew"

    # This is the whole point: neither may ever be renamed to *_chain.dll and chained to.
    Check 'neither is misclassified as third-party' `
          ((Test-IsOurProxy -Path $older) -and (Test-IsOurProxy -Path $newer)) ''

    # ---------------------------------------------------------------- the authority trap
    Write-Host ''
    Write-Host '  -- a bare substring is not authority to overwrite --'

    Check 'a text file containing the marker is rejected' `
          ($null -eq (Get-ProxyMarkerVersion -Path $textFile)) 'not a 32-bit PE'
    Check 'a PE whose marker is not NUL-terminated is rejected' `
          ($null -eq (Get-ProxyMarkerVersion -Path $unterm)) 'fails the C-string check'
    Check 'an unmarked third-party 32-bit DLL is rejected' `
          ($null -eq (Get-ProxyMarkerVersion -Path $plainPe)) 'no marker'
    Check 'a truncated file is rejected' `
          ($null -eq (Get-ProxyMarkerVersion -Path $truncated)) 'no PE header'
    Check 'a missing file is rejected' `
          ($null -eq (Get-ProxyMarkerVersion -Path (Join-Path $tmp 'absent.dll'))) ''

    # An empty prefix would make every file match, so it must fail loudly, not open.
    $saved = $script:ProxyMarkerPrefix
    $script:ProxyMarkerPrefix = ''
    $threw = $false
    try { Get-ProxyMarkerVersion -Path $plainPe | Out-Null } catch { $threw = $true }
    $script:ProxyMarkerPrefix = $saved
    Check 'an empty marker prefix throws rather than matching everything' $threw ''

    # ---------------------------------------------------------------- shim discovery
    Write-Host ''
    Write-Host '  -- shim database contents are read, not guessed --'

    # A database carrying a shim this tool has never heard of. The old implementation
    # searched the bytes for the two names it already hardcoded, so it was structurally
    # incapable of producing this result no matter what the file contained.
    $sdb3 = Join-Path $tmp 'three.sdb'
    New-TestSdb -Path $sdb3 -DbName 'Reissued DB' -Exe 'BGE.exe' `
                -Shims @('IgnoreAltTab', 'SingleProcAffinity', 'SomeFutureShim')

    $c = Get-SdbContents -Path $sdb3
    Check 'a database is parsed rather than pattern-matched' ($c.Ok) "name='$($c.Name)'"
    Check 'an UNHARDCODED third shim is discovered' `
          (@($c.Shims) -contains 'SomeFutureShim') ("shims: " + (@($c.Shims) -join ', '))
    Check 'all three shims are reported' (@($c.Shims).Count -eq 3) "$(@($c.Shims).Count) found"
    Check 'the patched executable is identified' `
          (@($c.Entries).Count -eq 1 -and $c.Entries[0].Exe -eq 'BGE.exe') ''

    # A database with nothing familiar in it at all.
    $sdbAlien = Join-Path $tmp 'alien.sdb'
    New-TestSdb -Path $sdbAlien -DbName 'Someone Else' -Exe 'Other.exe' -Shims @('WinXPSP3', 'Win8RTMVersionLie')
    $a = Get-SdbContents -Path $sdbAlien
    Check 'an unrelated database reports its real contents' `
          ($a.Ok -and (@($a.Shims) -contains 'Win8RTMVersionLie') -and -not (@($a.Shims) -contains 'IgnoreAltTab')) `
          ("shims: " + (@($a.Shims) -join ', '))

    # Unreadable must mean UNKNOWN, never "empty" - the caller refuses to remove on this.
    $junk = Join-Path $tmp 'junk.sdb'
    [System.IO.File]::WriteAllBytes($junk, [byte[]](1, 2, 3, 4, 5, 6, 7, 8))
    $j = Get-SdbContents -Path $junk
    Check 'an unparseable database reports UNKNOWN, not empty' (-not $j.Ok) "Ok=$($j.Ok)"

    $m = Get-SdbContents -Path (Join-Path $tmp 'absent.sdb')
    Check 'a missing database reports UNKNOWN, not empty' (-not $m.Ok) "Ok=$($m.Ok)"

    # What the tool tells the user it will destroy.
    # @() on the call: a one-element array returned from a function unwraps to a scalar,
    # and indexing a scalar string yields its first character.
    $unacc = @(Get-UnaccountedShims -Shims @($c.Shims))
    Check 'shims with no replacement are flagged as unaccounted for' `
          (@($unacc).Count -eq 1 -and $unacc[0] -eq 'SomeFutureShim') ("would be lost: " + (@($unacc) -join ', '))
    Check 'the two known shims are not flagged' `
          (@(Get-UnaccountedShims -Shims @('IgnoreAltTab', 'SingleProcAffinity')).Count -eq 0) ''

    # The -Status inventory only renders when a database is installed, so drive it with a
    # stub rather than leave that path unexercised on a machine that has none.
    Write-Host ''
    Write-Host '  -- -Status renders the inventory --'
    # Must carry every property Show-Status reads: under StrictMode a missing one is a
    # terminating error, so a stub that is merely close hides real breakage.
    function Get-BgeShimDatabase {
        [pscustomobject]@{
            Guid = '{test}'; Path = 'C:\test.sdb'; Description = 'Reissued DB'
            Readable = $true; IsOurs = $false; Shims = @($c.Shims); UnknownShims = @($unacc)
            HasBadShim = $true; Entries = @($c.Entries)
        }
    }
    function Get-State { $null }
    $rendered = (Show-Status -Folder $tmp 6>&1 | Out-String)
    Check 'every shim in the database is listed' `
          (($rendered -match 'IgnoreAltTab') -and ($rendered -match 'SingleProcAffinity') -and
           ($rendered -match 'SomeFutureShim')) ''
    Check 'the unaccounted-for shim is called out' `
          ($rendered -match 'NOT accounted for by this tool') ''
    Check 'the executable is named alongside its shims' ($rendered -match 'BGE\.exe : ') ''

    function Get-BgeShimDatabase {
        [pscustomobject]@{
            Guid = '{test}'; Path = 'C:\bad.sdb'; Description = 'Unreadable'
            Readable = $false; IsOurs = $false; Shims = @(); UnknownShims = @()
            HasBadShim = $false; Entries = @()
        }
    }
    $rendered = (Show-Status -Folder $tmp 6>&1 | Out-String)
    Check 'an unreadable database is reported as unknown, not empty' `
          ($rendered -match 'COULD NOT BE READ') ''

    # state.json is data on disk and may be older, truncated or hand-edited. Under
    # StrictMode a missing property is a terminating error, so reading it has to be
    # guarded - a revert must not die precisely when the state file matters most.
    Check 'a missing state field reads as empty, not an error' `
          ((Get-StateValue $null 'Anything') -eq '' -and
           (Get-StateValue ([pscustomobject]@{ A = 1 }) 'B') -eq '' -and
           (Get-StateValue ([pscustomobject]@{ A = $null }) 'A') -eq '') ''
    Check 'a present state field is returned' `
          ((Get-StateValue ([pscustomobject]@{ ShortcutPath = 'C:\x.lnk' }) 'ShortcutPath') -eq 'C:\x.lnk') ''

    function Get-State { [pscustomobject]@{ GamePath = 'C:\somewhere' } }   # no ShortcutPath
    $ok = $true
    try { $null = Show-Status -Folder $tmp 6>&1 | Out-String } catch { $ok = $false }
    Check 'a partial state file does not crash -Status' $ok ''
    function Get-State { $null }

    # Against the genuine article, when this machine has one.
    $script:goggame = 'C:\GOG Games\Beyond Good and Evil\goggame.sdb'
    $goggame = $script:goggame
    if (Test-Path -LiteralPath $goggame) {
        $g = Get-SdbContents -Path $goggame
        Check "the real GOG database parses" `
              ($g.Ok -and (@($g.Shims) -contains 'IgnoreAltTab') -and (@($g.Shims) -contains 'SingleProcAffinity')) `
              ("$($g.Name): " + (@($g.Shims) -join ', '))
    }
    else {
        Write-Host '  SKIP no goggame.sdb on this machine to check against'
    }

    # ---------------------------------------------------------------- derivation
    Write-Host ''
    Write-Host '  -- deriving an affinity-only database --'

    # Every index entry is [8-byte key][DWORD TAGID] pointing at a tag elsewhere in the
    # file. Splicing bytes out invalidates any TAGID past the cut, and a database whose
    # index points at the wrong offset installs happily and then never matches - the
    # silent failure that would be worse than the shortcut it replaces.
    function Test-SdbIndexIntegrity {
        param([string]$Path)
        $bytes = [System.IO.File]::ReadAllBytes($Path)
        $nodes = Get-SdbNodes -Bytes $bytes
        if (-not $nodes) { return $false }
        $byPos = @{}
        foreach ($n in $nodes) { $byPos[$n.Pos] = $n }

        foreach ($bits in ($nodes | Where-Object { $_.Tag -eq 0x9801 })) {
            $idx = $nodes | Where-Object { $_.Index -eq $bits.Parent } | Select-Object -First 1
            if (-not $idx) { continue }
            $wantTag = $null
            foreach ($c in $nodes) {
                if ($c.Parent -eq $idx.Index -and $c.Tag -eq 0x3802) {
                    $wantTag = [BitConverter]::ToUInt16($bytes, $c.Pos + 2)
                }
            }
            $at = $bits.Pos + $bits.HdrSize
            for ($i = 0; $i + 12 -le $bits.DataSize; $i += 12) {
                $tid = [BitConverter]::ToUInt32($bytes, $at + $i + 8)
                if (-not $byPos.ContainsKey([int]$tid)) { return $false }
                if ($null -ne $wantTag -and $byPos[[int]$tid].Tag -ne $wantTag) { return $false }
            }
        }
        return $true
    }

    $derived = Join-Path $tmp 'derived.sdb'
    $r = New-AffinityOnlySdb -SourcePath $sdb3 -DestPath $derived -RemoveShim 'IgnoreAltTab' `
                             -NewGuid $script:AffinityDbGuid -NewName $script:AffinityDbName
    Check 'a database can be derived with the bad shim removed' ($r.Ok) $r.Reason
    Check 'IgnoreAltTab is gone from the result' (-not (@($r.Shims) -contains 'IgnoreAltTab')) `
          ("kept: " + (@($r.Shims) -join ', '))
    Check 'SingleProcAffinity survives' (@($r.Shims) -contains 'SingleProcAffinity') ''
    Check 'unrelated shims are preserved, not quietly dropped' `
          (@($r.Shims) -contains 'SomeFutureShim') ''
    Check 'the result still parses through apphelp' ((Get-SdbContents -Path $derived).Ok) ''
    Check 'the result is renamed to ours' `
          ((Get-SdbContents -Path $derived).Name -eq $script:AffinityDbName) ''
    Check 'every index TAGID still resolves to the right tag' (Test-SdbIndexIntegrity -Path $derived) ''

    # The database GUID must be ours, or -Revert could not tell it from GOG's.
    $db = [System.IO.File]::ReadAllBytes($derived)
    $nodes = Get-SdbNodes -Bytes $db
    $idNode = $nodes | Where-Object { $_.Tag -eq 0x9007 -and $_.DataSize -eq 16 } | Select-Object -First 1
    Check 'the result carries a database GUID at all' ($null -ne $idNode) ''
    if ($idNode) {
        $g = New-Object byte[] 16
        [Array]::Copy($db, $idNode.Pos + $idNode.HdrSize, $g, 0, 16)
        Check 'the GUID is stamped as ours, not the source database''s' `
              (([guid]$g) -eq $script:AffinityDbGuid) ([guid]$g).ToString()
    }

    # Refuses rather than producing something that looks derived but is not.
    $none = New-AffinityOnlySdb -SourcePath $sdbAlien -DestPath (Join-Path $tmp 'x.sdb') `
                                -RemoveShim 'IgnoreAltTab' -NewGuid $script:AffinityDbGuid -NewName 'x'
    Check 'refuses when the shim to remove is not present' (-not $none.Ok) $none.Reason

    # The two parsers - apphelp for reading, the raw walker for surgery - must agree.
    $viaApphelp = @((Get-SdbContents -Path $sdb3).Shims | Sort-Object)
    $bytes3 = [System.IO.File]::ReadAllBytes($sdb3)
    $n3 = Get-SdbNodes -Bytes $bytes3
    $stBase = ($n3 | Where-Object { $_.Tag -eq 0x7801 } | Select-Object -First 1)
    $stBase = $stBase.Pos + $stBase.HdrSize
    $viaRaw = @()
    foreach ($n in $n3) {
        if ($n.Tag -ne 0x7009) { continue }
        foreach ($c in $n3) {
            if ($c.Parent -eq $n.Index -and $c.Tag -eq 0x6001) {
                $viaRaw += Read-SdbRawString -Bytes $bytes3 -Base $stBase -Ref ([BitConverter]::ToUInt32($bytes3, $c.Pos + 2))
            }
        }
    }
    $viaRaw = @($viaRaw | Sort-Object)
    Check 'both SDB parsers agree on what a database applies' `
          (($viaApphelp -join '|') -eq ($viaRaw -join '|')) ("raw: " + ($viaRaw -join ', '))

    # And against the genuine article.
    if (Test-Path -LiteralPath $goggame) {
        $realDerived = Join-Path $tmp 'real_derived.sdb'
        $rr = New-AffinityOnlySdb -SourcePath $goggame -DestPath $realDerived -RemoveShim 'IgnoreAltTab' `
                                  -NewGuid $script:AffinityDbGuid -NewName $script:AffinityDbName
        Check 'the real GOG database derives cleanly' ($rr.Ok) $rr.Reason
        Check 'the derived GOG database applies only SingleProcAffinity' `
              ((@($rr.Shims).Count -eq 1) -and (@($rr.Shims)[0] -eq 'SingleProcAffinity')) `
              ("applies: " + (@($rr.Shims) -join ', '))
        Check 'the derived GOG database keeps a valid index' (Test-SdbIndexIntegrity -Path $realDerived) ''
        $g2 = Get-SdbContents -Path $realDerived
        Check 'the derived GOG database still targets BGE.exe' `
              ((@($g2.Entries).Count -eq 1) -and ($g2.Entries[0].Exe -eq 'BGE.exe')) ''
    }

    # ---------------------------------------------------------------- argument quoting
    Write-Host ''
    Write-Host '  -- command line quoting for the elevated relaunch --'

    # Asserted against the real parser, CommandLineToArgvW, rather than against my reading
    # of the rules. A -GamePath ending in a backslash - what drag-and-drop and tab
    # completion both produce - used to escape its own closing quote.
    Add-Type -Namespace BgeFixTest -Name Argv -ErrorAction SilentlyContinue -MemberDefinition @'
[DllImport("shell32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
public static extern IntPtr CommandLineToArgvW(string lpCmdLine, out int pNumArgs);
[DllImport("kernel32.dll")]
public static extern IntPtr LocalFree(IntPtr hMem);
'@
    function Get-ParsedArgs {
        param([string]$CommandLine)
        $n = 0
        $p = [BgeFixTest.Argv]::CommandLineToArgvW($CommandLine, [ref]$n)
        if ($p -eq [IntPtr]::Zero) { return @() }
        try {
            $out = @()
            for ($i = 0; $i -lt $n; $i++) {
                $sp = [System.Runtime.InteropServices.Marshal]::ReadIntPtr($p, $i * [IntPtr]::Size)
                $out += [System.Runtime.InteropServices.Marshal]::PtrToStringUni($sp)
            }
            return $out
        }
        finally { [void][BgeFixTest.Argv]::LocalFree($p) }
    }

    $cases = @(
        'D:\Games\BGE\',                       # the reported break: trailing backslash
        'D:\Games\BGE',
        'C:\GOG Games\Beyond Good and Evil',   # spaces
        'C:\GOG Games\Beyond Good and Evil\',  # spaces and a trailing backslash
        'C:\odd\path\\',                       # two trailing backslashes
        'C:\a b\c""d',                         # embedded quotes
        'plain',
        ''
    )
    $bad = @()
    foreach ($case in $cases) {
        $line = 'app.exe -GamePath ' + (ConvertTo-ProcessArgument $case) + ' -Force'
        $parsed = @(Get-ParsedArgs -CommandLine $line)
        if ($parsed.Count -ne 4 -or $parsed[2] -ne $case -or $parsed[3] -ne '-Force') {
            $bad += "'$case' -> [$(@($parsed) -join '] [')]"
        }
    }
    Check 'every path survives a round trip through CommandLineToArgvW' `
          ($bad.Count -eq 0) $(if ($bad.Count) { $bad -join ' ; ' } else { "$($cases.Count) cases" })

    # The specific regression, called out on its own so a failure names itself.
    $line = 'app.exe -GamePath ' + (ConvertTo-ProcessArgument 'D:\Games\BGE\') + ' -Force'
    $parsed = @(Get-ParsedArgs -CommandLine $line)
    Check 'a trailing backslash no longer escapes its closing quote' `
          ($parsed.Count -eq 4 -and $parsed[2] -eq 'D:\Games\BGE\') `
          ("parsed: [$(@($parsed) -join '] [')]")

    # And confirm the test is not vacuous: the old naive quoting really does break here.
    $naive = @(Get-ParsedArgs -CommandLine 'app.exe -GamePath "D:\Games\BGE\" -Force')
    Check 'the naive quoting this replaced is genuinely broken' `
          ($naive.Count -ne 4 -or $naive[2] -ne 'D:\Games\BGE\') `
          ("naive: [$(@($naive) -join '] [')]")

    # ---------------------------------------------------------------- elevation contract
    Write-Host ''
    Write-Host '  -- the elevated relaunch --'

    # Stubbed so this runs with no UAC prompt: what matters is that the parent waits,
    # propagates the child's exit code, and forwards the right arguments.
    $script:capturedArgs = $null
    function Start-Process {
        param($FilePath, $ArgumentList, $Verb, [switch]$PassThru, [switch]$Wait)
        $script:capturedArgs = $ArgumentList
        $p = New-Object psobject -Property @{ ExitCode = 3 }
        $p | Add-Member -MemberType ScriptMethod -Name WaitForExit -Value { } -Force
        return $p
    }

    $bound = @{ GamePath = 'D:\Games\BGE\'; Force = [switch]$true; NoElevate = [switch]$true }
    $code = Invoke-SelfElevate -Bound $bound 6>&1 | Select-Object -Last 1
    $code = [int]("$code")

    Check "the child's exit code is propagated, not swallowed" ($code -eq 3) "got $code"

    $args = @($script:capturedArgs)
    Check 'the child is told it was relaunched' ($args -contains '-ElevatedRelaunch') ''
    Check '-NoExit is not used, so -Force can run unattended' (-not ($args -contains '-NoExit')) ''
    Check '-NoElevate is not forwarded, so the child cannot bounce' (-not ($args -contains '-NoElevate')) ''
    Check 'switches are forwarded without a value' ($args -contains '-Force') ''

    $gp = [array]::IndexOf($args, '-GamePath')
    Check 'a trailing-backslash GamePath is forwarded quoted correctly' `
          ($gp -ge 0 -and (@(Get-ParsedArgs -CommandLine ('x.exe ' + $args[$gp+1]))[1]) -eq 'D:\Games\BGE\') `
          $(if ($gp -ge 0) { $args[$gp+1] } else { 'absent' })

    # A multi-valued -Component cannot cross the UAC boundary as a list: powershell.exe -File
    # binds every argument as one literal string. It used to be forwarded as -Component with
    # [string] applied to the array, i.e. 'AltTab Windowed', so the elevated child failed
    # ValidateSet - in its own window, after the parent had printed the plan.
    $script:capturedArgs = $null
    $null = Invoke-SelfElevate -Bound @{ Component = @('AltTab', 'Windowed') } 6>&1
    $args2 = @($script:capturedArgs)
    $cl = [array]::IndexOf($args2, '-ComponentList')
    Check 'a component list is forwarded as one string, not as -Component' `
          (($cl -ge 0) -and -not ($args2 -contains '-Component')) `
          ("forwarded: $($args2 -join ' ')")
    Check 'the forwarded list keeps both components' `
          ($cl -ge 0 -and (@(Get-ParsedArgs -CommandLine ('x.exe ' + $args2[$cl+1]))[1]) -eq 'AltTab,Windowed') `
          $(if ($cl -ge 0) { $args2[$cl+1] } else { 'absent' })

    # Round trip: what the parent sends, the child must accept. Asserted by really running it.
    $savedEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $null = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repo 'Fix-BGE.ps1') `
                    -ComponentList 'AltTab,Windowed' -WhatIf 2>&1
        $childCode = $LASTEXITCODE
        # And confirm the test is not vacuous: the form this replaced really is rejected.
        $null = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repo 'Fix-BGE.ps1') `
                    -Component 'AltTab Windowed' -WhatIf 2>&1
        $naiveCode = $LASTEXITCODE
    }
    finally { $ErrorActionPreference = $savedEap }

    Check 'the elevated child accepts the forwarded component list' ($childCode -eq 0) "exit $childCode"
    Check 'the space-joined form this replaced is genuinely rejected' ($naiveCode -ne 0) "exit $naiveCode"

    Check '-ComponentList splits back into a component list' `
          ((@(Resolve-ComponentSpec -Spec 'AltTab,Windowed') -join '|') -eq 'AltTab|Windowed') `
          (@(Resolve-ComponentSpec -Spec 'AltTab,Windowed') -join '|')
    Check '-ComponentList tolerates the spacing a human would add' `
          ((@(Resolve-ComponentSpec -Spec ' AltTab , Controller ') -join '|') -eq 'AltTab|Controller') ''

    $threw = $false
    try { Resolve-ComponentSpec -Spec 'AltTab,Nonsense' } catch { $threw = $true }
    Check '-ComponentList rejects a name that is not a component' $threw ''

    $threw = $false
    try { Resolve-ComponentSpec -Spec ',,' } catch { $threw = $true }
    Check '-ComponentList rejects an empty list' $threw ''

    # ---------------------------------------------------------------- component model
    Write-Host ''
    Write-Host '  -- component selection --'

    Check 'installing defaults to AltTab and Windowed' `
          ((@(Resolve-Components) -join ',') -eq 'AltTab,Windowed') `
          (@(Resolve-Components) -join ',')
    Check 'All means all three' `
          (@(Resolve-Components -Requested @('All')).Count -eq 3) ''
    Check 'controller support can be installed on its own' `
          ((@(Resolve-Components -Requested @('Controller')) -join ',') -eq 'Controller') ''
    Check 'a revert defaults to everything' `
          (@(Resolve-Components -ForRevert).Count -eq 3) ''

    # Tuning for a component that is not being installed must be an error, not ignored.
    $threw = $false
    try { Assert-ComponentOptions -Components @('Controller') -Bound @{ WindowMode = 2 } }
    catch { $threw = $true }
    Check '-WindowMode is rejected when Windowed is not selected' $threw ''

    $threw = $false
    try { Assert-ComponentOptions -Components @('Windowed') -Bound @{ PadLookSpeed = 900 } }
    catch { $threw = $true }
    Check '-PadLookSpeed is rejected when Controller is not selected' $threw ''

    $ok = $true
    try { Assert-ComponentOptions -Components @('Windowed', 'Controller') -Bound @{ WindowMode = 2; PadLookSpeed = 900 } }
    catch { $ok = $false }
    Check 'tuning is accepted when its component is selected' $ok ''

    # ---------------------------------------------------------------- the selector
    Write-Host ''
    Write-Host '  -- the interactive selector --'

    # Read-Host is stubbed with a queue of answers: a function beats the cmdlet in name
    # resolution, so the REAL selector runs here - drawing, looping and toggling exactly as
    # it does for a user. The alternative was a second implementation in the test, which is
    # the thing that drifts. Show-Status gets a counter instead of running for real; the real
    # one is exercised earlier in this same file.
    $script:answers     = New-Object System.Collections.Queue
    $script:statusCalls = 0
    function Read-Host {
        param([string]$Prompt)
        if ($script:answers.Count -eq 0) {
            throw 'the selector asked more questions than the test had answers for'
        }
        return $script:answers.Dequeue()
    }
    function Show-Status { param([string]$Folder) $script:statusCalls++ }
    function Get-BgeShimDatabase { return $null }

    function Invoke-Selector {
        param([string[]]$Keys, [string[]]$Preselected = @('AltTab', 'Windowed'))
        $script:answers = New-Object System.Collections.Queue
        foreach ($k in $Keys) { $script:answers.Enqueue($k) }
        return (Show-ComponentSelector -Folder $tmp -Preselected $Preselected 6>$null)
    }

    $r = Invoke-Selector -Keys @('')
    Check 'Enter installs the preselected default' `
          ($r.Action -eq 'Install' -and (@($r.Components) -join ',') -eq 'AltTab,Windowed') `
          ("$($r.Action): $(@($r.Components) -join ',')")

    $r = Invoke-Selector -Keys @('3', '')
    Check 'a number turns its fix on' `
          ((@($r.Components) -join ',') -eq 'AltTab,Windowed,Controller') (@($r.Components) -join ',')

    $r = Invoke-Selector -Keys @('13', '')
    Check 'several numbers at once each toggle' `
          ((@($r.Components) -join ',') -eq 'Windowed,Controller') (@($r.Components) -join ',')

    $r = Invoke-Selector -Keys @('a', '')
    Check 'A selects all three' ((@($r.Components) -join ',') -eq 'AltTab,Windowed,Controller') ''

    # The one outcome that must be impossible: an install that was asked for and does nothing.
    $r = Invoke-Selector -Keys @('n', '', 'q')
    Check 'an empty selection cannot be installed' ($r.Action -eq 'Quit') $r.Action

    $r = Invoke-Selector -Keys @('q')
    Check 'Q quits without installing anything' ($r.Action -eq 'Quit') $r.Action

    $r = Invoke-Selector -Keys @('u', 'y')
    Check 'U asks, and a yes returns a revert' ($r.Action -eq 'Revert') $r.Action

    $r = Invoke-Selector -Keys @('u', 'n', 'q')
    Check 'U does nothing without a yes' ($r.Action -eq 'Quit') $r.Action

    $script:statusCalls = 0
    $r = Invoke-Selector -Keys @('s', '')
    Check 'S shows the current state and keeps the selection' `
          ($script:statusCalls -eq 1 -and (@($r.Components) -join ',') -eq 'AltTab,Windowed') `
          "status calls: $script:statusCalls"

    $r = Invoke-Selector -Keys @('x', '')
    Check 'an unrecognised answer changes nothing' `
          ((@($r.Components) -join ',') -eq 'AltTab,Windowed') (@($r.Components) -join ',')

    # Every key the selector returns has to be a name -Component accepts, or the choice dies
    # at the elevation boundary instead of installing.
    $r = Invoke-Selector -Keys @('a', '')
    $declaredSet = (Get-Command (Join-Path $repo 'Fix-BGE.ps1')).Parameters['Component'].Attributes |
                   Where-Object { $_ -is [System.Management.Automation.ValidateSetAttribute] }
    $validNames = @($declaredSet.ValidValues)
    $unknown = @(@($r.Components) | Where-Object { $validNames -notcontains $_ })
    Check 'the selector only returns names -Component accepts' (@($unknown).Count -eq 0) `
          $(if (@($unknown).Count) { "not accepted: $($unknown -join ', ')" } else { $validNames -join ',' })

    # The promise that keeps unattended runs working: no console on the other end means no
    # question. Asserted in a child process with a genuinely redirected stdin, because in this
    # one the answer depends on how the suite itself was started - from a real console it is
    # legitimately $true, so asserting on the harness's own stdin would fail for a developer
    # and pass in CI, which is the wrong way round.
    $probePath = Join-Path $tmp 'canprompt.ps1'
    Set-Content -LiteralPath $probePath -Value @(
        "`$ast = [System.Management.Automation.Language.Parser]::ParseFile('$src', [ref]`$null, [ref]`$null)"
        "`$f = `$ast.FindAll({ param(`$n) `$n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and `$n.Name -eq 'Test-CanPrompt' }, `$true)"
        ". ([scriptblock]::Create(`$f[0].Extent.Text))"
        "'CANPROMPT=' + (Test-CanPrompt)"
    )
    $probeOut = (& cmd /c "powershell -NoProfile -ExecutionPolicy Bypass -File ""$probePath"" < NUL" 2>&1) | Out-String
    Check 'a redirected stdin is never asked a question' ($probeOut -match 'CANPROMPT=False') $probeOut.Trim()

    # The labels come from the same files Show-Status reads, so they cannot disagree with it.
    $probe = Join-Path $tmp 'stateprobe'
    New-Item -ItemType Directory -Path $probe | Out-Null
    $st = Get-ComponentInstallState -Folder $probe
    Check 'a clean folder reports nothing installed' `
          ((-not $st.AltTab) -and (-not $st.Windowed) -and (-not $st.Controller)) ''
    Set-Content -LiteralPath (Join-Path $probe 'd3d9_windowed.ini')  -Value 'Mode=1'
    Set-Content -LiteralPath (Join-Path $probe 'dinput8_xinput.ini') -Value 'LookSpeed=1800'
    $st = Get-ComponentInstallState -Folder $probe
    Check 'an installed proxy is reported as installed' ($st.Windowed -and $st.Controller) ''

    # ---------------------------------------------------------------- parameter sets
    Write-Host ''
    Write-Host '  -- contradictory invocations fail at binding --'

    # These used to be accepted and silently do something else: -Status -Revert ran Status
    # and dropped the Revert; -Revert -WindowMode 2 took a setting for work it never did.
    $script = Join-Path $repo 'Fix-BGE.ps1'
    $contradictions = @(
        @{ Args = @('-Status', '-Revert');              Why = 'Status + Revert' }
        @{ Args = @('-Revert', '-WindowMode', '2');     Why = 'Revert + apply-only tuning' }
        @{ Args = @('-Status', '-PadLookSpeed', '900'); Why = 'Status + apply-only tuning' }
        # The removed selection switches. Rejected loudly rather than silently remapped.
        @{ Args = @('-NoShortcut');                     Why = 'removed -NoShortcut' }
        @{ Args = @('-NoWindowedProxy');                Why = 'removed -NoWindowedProxy' }
        @{ Args = @('-InstallControllerSupport');       Why = 'removed -InstallControllerSupport' }
    )
    # A rejected invocation writes to stderr, which this script's ErrorActionPreference
    # would otherwise treat as fatal - the expected outcome must not abort the test.
    $savedEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $accepted = @()
        foreach ($c in $contradictions) {
            $null = & powershell -NoProfile -ExecutionPolicy Bypass -File $script @($c.Args) 2>&1
            if ($LASTEXITCODE -eq 0) { $accepted += $c.Why }
        }
        $null = & powershell -NoProfile -ExecutionPolicy Bypass -File $script -WhatIf -Component Controller 2>&1
        $legitCode = $LASTEXITCODE
    }
    finally { $ErrorActionPreference = $savedEap }

    Check 'every contradictory or removed switch is rejected' ($accepted.Count -eq 0) `
          $(if ($accepted.Count) { "accepted: $($accepted -join '; ')" } else { "$($contradictions.Count) cases" })
    Check 'installing only the controller component is accepted' ($legitCode -eq 0) "exit $legitCode"

    # The removed switches must be gone from the declaration, not merely unreachable.
    $declared = (Get-Command $script).Parameters.Keys
    $stale = @('NoShortcut', 'NoWindowedProxy', 'InstallControllerSupport') |
             Where-Object { $declared -contains $_ }
    Check 'the removed switches are not declared at all' (@($stale).Count -eq 0) `
          $(if (@($stale).Count) { "still present: $($stale -join ', ')" } else { 'none' })

    Write-Host ''
    if ($script:fail) { Write-Host "  FAILED ($($script:fail) failures)" }
    else              { Write-Host '  ALL PASS (0 failures)' }
}
finally {
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}

exit $script:fail
