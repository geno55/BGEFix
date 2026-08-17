<#
.SYNOPSIS
    Packages a BGEFix release zip, and checks the payload before it ships.

.DESCRIPTION
    Produces out\BGEFix-<version>.zip plus a .sha256 beside it, containing exactly the
    files a user needs: the installer, the double-click entry point, both prebuilt proxies,
    the README and the licence. Source is not in the zip - GitHub attaches it separately.

    The checks are the point. A release is the one build nobody can fix in place once it is
    downloaded, and every one of these has a failure mode that reaches the user silently:

      - The version in the zip name must match the version Fix-BGE.ps1 prints. Otherwise a
        bug report names a build that never existed.
      - Both DLLs must be 32-bit PEs. BGE.exe is a 32-bit process and silently fails to
        load a 64-bit DLL, so a release built from the wrong toolchain looks fine, installs
        fine, and does nothing.
      - Both DLLs must carry the installer's own proxy marker. Without it the installer
        does not recognise its own DLL: on the next run it treats it as a third-party
        wrapper, renames it to *_chain.dll and chains itself to it.
      - The zip must contain the whole payload and nothing else, verified by reading the
        archive back rather than by trusting the copy that produced it.

    The DLL checks are lifted out of Fix-BGE.ps1 by the PowerShell parser rather than
    reimplemented here, so the release gate and the installer cannot disagree about what a
    valid proxy is.

.PARAMETER Version
    Version to package, with or without a leading 'v'. Defaults to the version declared in
    Fix-BGE.ps1. When given, it must match that version.

.PARAMETER OutDir
    Where to write the zip. Defaults to out\ in the repository root.

.EXAMPLE
    tools\Build-Release.ps1

.EXAMPLE
    # What the release workflow runs, with the tag it was triggered by.
    tools\Build-Release.ps1 -Version v1.0.0
#>
#Requires -Version 5.1
[CmdletBinding()]
param(
    [string] $Version,
    [string] $OutDir
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$repo      = Split-Path -Parent $PSScriptRoot
$installer = Join-Path $repo 'Fix-BGE.ps1'

# The payload. Paths are relative to the repository root and are preserved in the zip:
# Fix-BGE.ps1 resolves its default proxies as dist\*.dll beside itself, so the layout here
# is not cosmetic - flattening it breaks the defaults.
$payload = @(
    'Fix-BGE.ps1'
    'BGEFix.cmd'
    'README.md'
    'LICENSE'
    'dist\d3d9.dll'
    'dist\dinput8.dll'
)

function Fail { param([string]$Message) throw $Message }

# --- lift the proxy checks out of the installer -------------------------------------
$ast  = [System.Management.Automation.Language.Parser]::ParseFile($installer, [ref]$null, [ref]$null)
$want = 'Test-Pe32', 'Get-ProxyMarkerVersion'
$fns  = $ast.FindAll({
    param($n)
    $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $want -contains $n.Name
}, $true)
if ($fns.Count -ne $want.Count) {
    Fail "expected to lift $($want.Count) functions out of Fix-BGE.ps1, got $($fns.Count)"
}
foreach ($f in $fns) { . ([scriptblock]::Create($f.Extent.Text)) }

# Every top-level $script: assignment, so the lifted functions see the constants they read
# instead of tripping StrictMode on an unset variable.
foreach ($line in (Select-String -Path $installer -Pattern '^\$script:\w+\s*=')) {
    . ([scriptblock]::Create($line.Line))
}

# --- version ------------------------------------------------------------------------
$declared = $script:AppVersion
if (-not $declared) { Fail 'Fix-BGE.ps1 does not declare $script:AppVersion.' }

if ($Version) {
    $asked = $Version.TrimStart('v', 'V')
    if ($asked -ne $declared) {
        Fail ("version mismatch: asked for $asked, but Fix-BGE.ps1 declares $declared. " +
              'Update $script:AppVersion, or tag the version the script actually reports.')
    }
}
$version = $declared
if ($version -notmatch '^\d+\.\d+\.\d+$') {
    Fail "'$version' is not a three-part version. Fix `$script:AppVersion in Fix-BGE.ps1."
}

$name = "BGEFix-v$version"
if (-not $OutDir) { $OutDir = Join-Path $repo 'out' }
$zipPath = Join-Path $OutDir "$name.zip"

Write-Host ''
Write-Host "  Packaging $name" -ForegroundColor White
Write-Host ''

# --- check the payload before staging it ---------------------------------------------
foreach ($rel in $payload) {
    if (-not (Test-Path -LiteralPath (Join-Path $repo $rel))) { Fail "missing from the repository: $rel" }
}

foreach ($rel in @('dist\d3d9.dll', 'dist\dinput8.dll')) {
    $dll = Join-Path $repo $rel
    if (-not (Test-Pe32 -Path $dll)) {
        Fail ("$rel is not a 32-bit PE. BGE.exe is a 32-bit process and silently fails to " +
              'load a 64-bit DLL - rebuild with src\build.cmd, which forces /MACHINE:X86.')
    }
    $marker = Get-ProxyMarkerVersion -Path $dll
    if ($null -eq $marker) {
        Fail ("$rel does not carry the installer's proxy marker, so the installer will not " +
              'recognise it as its own and will chain to it instead of replacing it.')
    }
    Write-Host ("  [+] {0,-18} 32-bit, marker v{1}, {2:N0} bytes" -f `
                (Split-Path -Leaf $rel), $marker, (Get-Item -LiteralPath $dll).Length) -ForegroundColor Green
}

# --- zip ----------------------------------------------------------------------------
# Both: ZipFile and the CreateEntryFromFile extensions live in the FileSystem assembly,
# ZipArchiveMode in the other one.
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
if (-not (Test-Path -LiteralPath $OutDir)) { New-Item -ItemType Directory -Path $OutDir -Force | Out-Null }
if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }

# Each entry is added by name, rather than by pointing CreateFromDirectory at a staging copy.
# Two reasons: there is no staging copy to get out of step with the repository, and the entry
# names are written the way the zip format specifies. .NET Framework's CreateFromDirectory
# uses the OS separator, so a nested path ships as 'dist\d3d9.dll' - one entry with a
# backslash in its name - which Windows tolerates and other extractors do not.
#
# Every name is prefixed with BGEFix-v<version>/ so extracting produces one folder instead of
# scattering six files into the user's Downloads.
$zipFile = [System.IO.Compression.ZipFile]::Open($zipPath, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($rel in $payload) {
        $entryName = "$name/" + ($rel -replace '\\', '/')
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $zipFile, (Join-Path $repo $rel), $entryName,
            [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
}
finally { $zipFile.Dispose() }

# --- verify the archive, by reading it back ------------------------------------------
$expected = @($payload | ForEach-Object { "$name/" + ($_ -replace '\\', '/') } | Sort-Object)
$zip = [System.IO.Compression.ZipFile]::OpenRead($zipPath)
try {
    $actual = @($zip.Entries | ForEach-Object { $_.FullName } | Sort-Object)

    $missing = @($expected | Where-Object { $actual -notcontains $_ })
    $extra   = @($actual   | Where-Object { $expected -notcontains $_ })
    if ($missing.Count -gt 0) { Fail "the zip is missing: $($missing -join ', ')" }
    if ($extra.Count   -gt 0) { Fail "the zip contains files that are not payload: $($extra -join ', ')" }

    foreach ($entry in $zip.Entries) {
        $rel  = ($entry.FullName -replace "^$([regex]::Escape($name))/", '') -replace '/', '\'
        $real = (Get-Item -LiteralPath (Join-Path $repo $rel)).Length
        if ($entry.Length -ne $real) {
            Fail "$($entry.FullName) is $($entry.Length) bytes in the zip, $real in the repository"
        }
    }
}
finally { $zip.Dispose() }

# --- checksum -----------------------------------------------------------------------
# The two-space 'hash *file' form, so sha256sum -c and CertUtil users can both check it.
$hash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLower()
$sha  = "$zipPath.sha256"
Set-Content -LiteralPath $sha -Value "$hash *$name.zip" -Encoding ASCII -NoNewline

Write-Host ''
Write-Host ("  [+] {0} ({1:N0} bytes, {2} files)" -f (Split-Path -Leaf $zipPath), `
            (Get-Item -LiteralPath $zipPath).Length, $expected.Count) -ForegroundColor Green
Write-Host "  [+] sha256 $hash" -ForegroundColor Green
Write-Host ''

# Emitted so a caller can pick the paths up without guessing at the naming.
[pscustomobject]@{
    Version  = $version
    Name     = $name
    Zip      = $zipPath
    Checksum = $sha
    Sha256   = $hash
}
