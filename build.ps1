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
    # The same folder zipped, for the release: NetISO Sync for other servers,
    # and the launchers the Docker image downloads (Dockerfile).
    Compress-Archive -Path $sync -DestinationPath build\NetISO-Sync.zip -Force
    # NetDisc.exe (NetDisc 360): the PC server that replaces netiso-srv and keeps the
    # Xbox's game folders current, with default.xex and boot.xex built in
    # (server/). Rebuilt every time so it carries the XEXs just made, and the
    # dashboard page (web/index.html, made into server/web_page.h).
    & python tools\make_web.py
    if ($LASTEXITCODE) { throw 'make_web.py failed.' }
    & $msbuild server\NetIsoServer.vcxproj /t:Rebuild /p:Configuration=Release /p:Platform=x64 /v:minimal /nologo
    if ($LASTEXITCODE) { throw 'Server compilation failed. It needs the Visual Studio "Desktop development with C++" workload.' }
    # NetIsoTiles.xex: the DashLaunch plugin that starts tiles from inside Aurora (plugin/main.cpp).
    # It is linked at its system base (0x91380000), which the public imagexex
    # refuses, so its PE header is patched to a title base for imagexex and
    # tools/sysdll.py then moves the XEX back to the system base, as a system
    # DLL like NetISO.xex. The code itself is never relocated.
    & $msbuild plugin\NetIsoTiles.vcxproj /t:Build /p:Configuration=Release '/p:Platform=Xbox 360' /v:minimal /nologo
    if ($LASTEXITCODE) { throw 'Plugin compilation failed.' }
    & python tools\sysdll.py pe-base build\xbox\NetIsoTiles.dll build\xbox\NetIsoTiles-title.dll 0x82000000
    if ($LASTEXITCODE) { throw 'sysdll.py pe-base failed.' }
    & $imagexex.FullName /IN:build\xbox\NetIsoTiles-title.dll /OUT:build\xbox\NetIsoTiles-title.xex /CONFIG:plugin\xex.xml
    if ($LASTEXITCODE) { throw 'imagexex failed for NetIsoTiles.' }
    & python tools\sysdll.py xex build\xbox\NetIsoTiles-title.xex build\NetIsoTiles.xex 0x91380000
    if ($LASTEXITCODE) { throw 'sysdll.py failed for NetIsoTiles.' }
    if ($Tests) {
        & python tests\test_generator.py
        if ($LASTEXITCODE) { throw 'Generator tests failed.' }
    }
    Write-Host "Built $sync. Copy it to Hdd1:\NetISO\NetISO Sync on the console."
    Write-Host 'Built build\NetIsoTiles.xex. Add it as a DashLaunch plugin, next to NetISO.xex.'
    Write-Host 'Built build\NetDisc.exe (NetDisc 360). Start it: it opens its dashboard in your browser. NetDisc.exe -h in a command prompt lists its options.'
} finally { Pop-Location }
