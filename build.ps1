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
    if ($Tests) {
        & python tests\test_generator.py
        if ($LASTEXITCODE) { throw 'Generator tests failed.' }
    }
    Write-Host "Built $sync. Copy it to Hdd1:\NetISO\NetISO Sync on the console."
} finally { Pop-Location }
