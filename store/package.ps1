# Makes the Xbox 360 Homebrew Store files for a release, in build\:
#   NetDisc360-<version>.7z   NetIsoTiles.xex, readme.txt and LICENSE.txt, which the
#                             store unpacks into Apps\NetDisc360\. NetISO.xex isn't
#                             ours to pass on, so readme.txt says where to get it.
#   NetDisc360-<version>.ini  the store entry for that 7z (from consolemods.ini).
# The version comes from server\version.h, as for the release (RELEASING.md).
# -Xex is the plugin to package: build\NetIsoTiles.xex from .\build.ps1, or the release's own.
param([string]$Xex = 'build\NetIsoTiles.xex', [string]$SevenZip)
$ErrorActionPreference = 'Stop'
Push-Location (Split-Path $PSScriptRoot)
try {
    $version = [regex]::Match((Get-Content -Raw server\version.h), '#define NETDISC_VERSION "(\d+\.\d+\.\d+)"').Groups[1].Value
    if (!$version) { throw 'No NETDISC_VERSION found in server\version.h.' }
    if (!(Test-Path -LiteralPath $Xex)) { throw "$Xex is missing. Run .\build.ps1 first, or pass -Xex with the release's NetIsoTiles.xex." }
    if (!$SevenZip) { $SevenZip = (Get-Command 7z -ErrorAction SilentlyContinue).Source }
    if (!$SevenZip) { $SevenZip = Join-Path $env:ProgramFiles '7-Zip\7z.exe' }
    if (!(Test-Path -LiteralPath $SevenZip)) { throw '7-Zip is needed. Install it, or pass -SevenZip with the path to 7z.exe.' }
    $name = "NetDisc360-$version"
    New-Item -ItemType Directory -Force build | Out-Null
    $out = (Resolve-Path build).Path
    # 7-Zip adds to an archive that exists, so the old files go first.
    foreach ($file in "$out\$name.7z", "$out\$name.ini") { if (Test-Path -LiteralPath $file) { Remove-Item -LiteralPath $file } }
    $stage = "build\store\$name"
    New-Item -ItemType Directory -Force $stage | Out-Null
    Copy-Item -LiteralPath $Xex -Destination "$stage\NetIsoTiles.xex" -Force
    Copy-Item -LiteralPath LICENSE -Destination "$stage\LICENSE.txt" -Force
    (Get-Content -Raw store\readme.txt).Replace('@VERSION@', $version) | Set-Content -NoNewline -Encoding ascii "$stage\readme.txt"
    # The files go at the top of the 7z, with no folder around them: the
    # store unpacks it into the entry's path itself. Default 7-Zip settings
    # (LZMA2), as the 7z files on the ConsoleMods repo use.
    Push-Location $stage
    try { & $SevenZip a -t7z -bso0 "$out\$name.7z" * } finally { Pop-Location }
    if ($LASTEXITCODE) { throw '7-Zip failed on the 7z.' }
    $size = (Get-Item -LiteralPath "$out\$name.7z").Length
    $sizeText = if ($size -ge 1MB) { ($size / 1MB).ToString('0.#', [Globalization.CultureInfo]::InvariantCulture) + 'MB' } else { [string][math]::Ceiling($size / 1KB) + 'KB' }
    (Get-Content -Raw store\consolemods.ini).Replace('@VERSION@', $version).Replace('@SIZE@', $sizeText) | Set-Content -NoNewline -Encoding ascii "$out\$name.ini"
    Write-Host "Made build\$name.7z ($sizeText) and build\$name.ini."
    Write-Host "Attach the 7z to the GitHub release v$version. The ini's dataurl points at it there."
} finally { Pop-Location }
