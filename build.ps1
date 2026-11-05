param([switch]$Tests)
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (!(Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer/vswhere is required.' }
    $vsPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$vsPath) { throw 'Install Visual Studio C++ build tools.' }
    $msbuild = Join-Path $vsPath 'MSBuild\Current\Bin\MSBuild.exe'
    # Which build this is (shared/build_id.h): the same for everything below.
    & python tools\make_version.py
    if ($LASTEXITCODE) { throw 'make_version.py failed.' }
    & $msbuild launcher\NetIsoLauncher.vcxproj /t:Build /p:Configuration=Release '/p:Platform=Xbox 360' /v:minimal /nologo
    if ($LASTEXITCODE) { throw 'Xbox compilation failed. Check the SDK and RXDK integration.' }
    & $msbuild launcher\NetDiscTile.vcxproj /t:Build /p:Configuration=Release '/p:Platform=Xbox 360' /v:minimal /nologo
    if ($LASTEXITCODE) { throw 'Xbox compilation of the tile launcher failed.' }
    # NetDiscParty.xex: the LAN party's raffle wheel on each console (party/),
    # in a folder of its own for Hdd1:\NetISO, where Aurora lists it.
    & $msbuild party\NetDiscParty.vcxproj /t:Build /p:Configuration=Release '/p:Platform=Xbox 360' /v:minimal /nologo
    if ($LASTEXITCODE) { throw 'Xbox compilation of NetDiscParty failed.' }
    $party = 'build\NetDisc360 Party'
    New-Item -ItemType Directory -Force $party | Out-Null
    Copy-Item -LiteralPath build\xbox\NetDiscParty.xex -Destination "$party\default.xex" -Force
    # The folder to copy to the console: Sync is the tile launcher with a
    # netiso.ini that has no iso=, and it copies that tile launcher into each
    # game folder it makes (see shared/helper.h).
    $sync = 'build\NetISO Sync'
    New-Item -ItemType Directory -Force $sync | Out-Null
    Copy-Item -LiteralPath build\xbox\NetDiscTile.xex -Destination "$sync\default.xex" -Force
    Copy-Item -LiteralPath launcher\sync.ini -Destination "$sync\netiso.ini" -Force
    # boot.xex: the full launcher without execution info (no Title ID), the
    # helper that mounts and starts the disc for every game's tile launcher.
    # See launcher/main.cpp.
    $sdks = @($env:XEDK, $env:RXDK, 'C:\Program Files (x86)\Microsoft Xbox 360 SDK', 'C:\Program Files\RXDK-360\bin') | Where-Object { $_ -and (Test-Path -LiteralPath $_) }
    $imagexex = Get-ChildItem -Recurse -Filter imagexex.exe -Path $sdks -ErrorAction SilentlyContinue | Select-Object -First 1
    if (!$imagexex) { throw 'imagexex.exe not found; set XEDK or RXDK.' }
    & $imagexex.FullName /IN:build\xbox\NetIsoLauncher.exe "/OUT:$sync\boot.xex" /CONFIG:launcher\xex.xml
    if ($LASTEXITCODE) { throw 'imagexex failed for boot.xex.' }
    # The same folder zipped, for the release: NetISO Sync for other servers,
    # and the launchers the Docker image downloads (Dockerfile).
    # Python's zipfile, not Compress-Archive: Windows PowerShell 5.1 writes
    # backslashes in the paths, which unzip on Linux warns about.
    & python -c "import shutil; shutil.make_archive('build/NetISO-Sync', 'zip', 'build', 'NetISO Sync')"
    if ($LASTEXITCODE) { throw 'Zipping NetISO Sync failed.' }
    # NetDisc360.exe: the PC server that replaces netiso-srv and keeps the
    # Xbox's game folders current, with the tile launcher (default.xex) and the helper (boot.xex) built in
    # (server/). Rebuilt every time so it carries the XEXs just made, and the
    # dashboard page (web/index.html, made into server/web_page.h) and the
    # System Link list (data/systemlink.json, made into server/systemlink.cpp).
    & python tools\make_web.py
    if ($LASTEXITCODE) { throw 'make_web.py failed.' }
    & python tools\make_systemlink.py
    if ($LASTEXITCODE) { throw 'make_systemlink.py failed.' }
    & $msbuild server\NetDisc.vcxproj /t:Rebuild /p:Configuration=Release /p:Platform=x64 /v:minimal /nologo
    if ($LASTEXITCODE) { throw 'Server compilation failed. It needs the Visual Studio "Desktop development with C++" workload.' }
    # NetDisc360.xex: the DashLaunch plugin, the console's disc drive for games
    # from the server (plugin/disc.cpp, in NetISO.xex's place) that also
    # handles tiles and disc swaps (plugin/main.cpp). NetIsoTiles.xex before.
    # It is linked at its system base (0x91C00000; not 0x91380000, which is
    # aac.xex's, loaded before the plugins on some consoles), which the public imagexex
    # refuses, so its PE header is patched to a title base for imagexex and
    # tools/sysdll.py then moves the XEX back to the system base, as a system
    # DLL like NetISO.xex. The code itself is never relocated.
    & $msbuild plugin\NetDisc360.vcxproj /t:Build /p:Configuration=Release '/p:Platform=Xbox 360' /v:minimal /nologo
    if ($LASTEXITCODE) { throw 'Plugin compilation failed.' }
    & python tools\sysdll.py pe-base build\xbox\NetDisc360.dll build\xbox\NetDisc360-title.dll 0x82000000
    if ($LASTEXITCODE) { throw 'sysdll.py pe-base failed.' }
    & $imagexex.FullName /IN:build\xbox\NetDisc360-title.dll /OUT:build\xbox\NetDisc360-title.xex /CONFIG:plugin\xex.xml
    if ($LASTEXITCODE) { throw 'imagexex failed for NetDisc360.xex.' }
    & python tools\sysdll.py xex build\xbox\NetDisc360-title.xex build\NetDisc360.xex 0x91C00000
    if ($LASTEXITCODE) { throw 'sysdll.py failed for NetDisc360.xex.' }
    if ($Tests) {
        & python tests\test_generator.py
        if ($LASTEXITCODE) { throw 'Generator tests failed.' }
    }
    Write-Host "Built $sync. Copy it to Hdd1:\Apps\NetISO Sync on the console."
    Write-Host "Built $party. Copy it to Hdd1:\NetDisc360\Games\NetDisc360 Party on each console for the LAN party's raffle wheel."
    Write-Host 'Built build\NetDisc360.xex. Add it as a DashLaunch plugin in place of NetIsoTiles.xex; it replaces NetISO.xex too.'
    Write-Host 'Built build\NetDisc360.exe. Start it: it opens its dashboard in your browser. NetDisc360.exe -h in a command prompt lists its options.'
} finally { Pop-Location }
