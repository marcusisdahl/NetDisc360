param([switch]$Tests)
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (!(Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer/vswhere is required.' }
    $vsPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$vsPath) { throw 'Install Visual Studio C++ build tools.' }
    $msbuild = Join-Path $vsPath 'MSBuild\Current\Bin\MSBuild.exe'
    & $msbuild launcher\NetIsoLauncher.vcxproj /t:Build /p:Configuration=Release '/p:Platform=Xbox 360' /v:minimal /nologo
    if ($LASTEXITCODE) { throw 'Xbox compilation failed. Check the SDK and RXDK integration.' }
    # The folder to copy to the console: Sync is the launcher XEX with a netiso.ini that has no iso=.
    $sync = 'build\NetISO Sync'
    New-Item -ItemType Directory -Force $sync | Out-Null
    Copy-Item -LiteralPath build\xbox\NetIsoLauncher.xex -Destination "$sync\default.xex" -Force
    Copy-Item -LiteralPath launcher\sync.ini -Destination "$sync\netiso.ini" -Force
    # boot.xex: the same program without execution info (no Title ID), which
    # mounts and starts the disc for each game's launcher. See launcher/main.cpp.
    $sdks = @($env:XEDK, $env:RXDK, 'C:\Program Files (x86)\Microsoft Xbox 360 SDK', 'C:\Program Files\RXDK-360\bin') | Where-Object { $_ -and (Test-Path -LiteralPath $_) }
    $imagexex = Get-ChildItem -Recurse -Filter imagexex.exe -Path $sdks -ErrorAction SilentlyContinue | Select-Object -First 1
    if (!$imagexex) { throw 'imagexex.exe not found; set XEDK or RXDK.' }
    & $imagexex.FullName /IN:build\xbox\NetIsoLauncher.exe "/OUT:$sync\boot.xex" /CONFIG:launcher\xex.xml
    if ($LASTEXITCODE) { throw 'imagexex failed for boot.xex.' }
    # NetIsoTiles.xex: the DashLaunch plugin that starts tiles from inside Aurora (plugin/main.cpp).
    & $msbuild plugin\NetIsoTiles.vcxproj /t:Build /p:Configuration=Release '/p:Platform=Xbox 360' /v:minimal /nologo
    if ($LASTEXITCODE) { throw 'Plugin compilation failed.' }
    Copy-Item -LiteralPath build\xbox\NetIsoTiles.xex -Destination build\NetIsoTiles.xex -Force
    if ($Tests) {
        & python tests\test_generator.py
        if ($LASTEXITCODE) { throw 'Generator tests failed.' }
    }
    Write-Host "Built $sync. Copy it to Hdd1:\NetISO\NetISO Sync on the console."
    Write-Host 'Built build\NetIsoTiles.xex. Add it as a DashLaunch plugin, next to NetISO.xex.'
} finally { Pop-Location }
