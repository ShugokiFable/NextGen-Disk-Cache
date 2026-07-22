# Build NextGenDiskCache 2.0.0 (DLL) into package\SKSE\Plugins.
# Robust across Visual Studio 2019/2022/2026: the CMake generator is derived
# from whatever vswhere reports, and DirectStorage 1.3 is fetched via nuget.exe
# when present or a direct nupkg download otherwise.
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $here

# --- DirectStorage 1.3 SDK (header + import lib) -----------------------------
$ds = Join-Path $here "deps\directstorage"
if (-not (Test-Path (Join-Path $ds "native\include\dstorage.h"))) {
    $nuget = Get-Command nuget.exe -ErrorAction SilentlyContinue
    if ($nuget) {
        & $nuget.Source install Microsoft.Direct3D.DirectStorage -Version 1.3.0 `
            -OutputDirectory (Join-Path $here "deps\_nuget") -NonInteractive
        if ($LASTEXITCODE -ne 0) { throw "nuget install failed: $LASTEXITCODE" }
        $pkg = Join-Path $here "deps\_nuget\Microsoft.Direct3D.DirectStorage.1.3.0"
        if (Test-Path $ds) { Remove-Item $ds -Recurse -Force }
        Copy-Item $pkg $ds -Recurse
    } else {
        # No nuget.exe: a .nupkg is a plain zip. Pull it straight from nuget.org.
        Write-Host "nuget.exe not found; downloading DirectStorage 1.3.0 nupkg directly"
        $url = "https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.directstorage/1.3.0/microsoft.direct3d.directstorage.1.3.0.nupkg"
        $tmpPkg = Join-Path $env:TEMP "ngdc_dstorage_1.3.0.nupkg"
        $tmpDir = Join-Path $env:TEMP "ngdc_dstorage_1.3.0"
        Invoke-WebRequest -Uri $url -OutFile $tmpPkg -UseBasicParsing
        if (Test-Path $tmpDir) { Remove-Item $tmpDir -Recurse -Force }
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        [System.IO.Compression.ZipFile]::ExtractToDirectory($tmpPkg, $tmpDir)
        New-Item -ItemType Directory -Force `
            (Join-Path $ds "native\include"), (Join-Path $ds "native\bin\x64"), (Join-Path $ds "native\lib\x64") | Out-Null
        Copy-Item (Join-Path $tmpDir "native\include\*.h")            (Join-Path $ds "native\include\")
        Copy-Item (Join-Path $tmpDir "native\bin\x64\dstorage*.dll")  (Join-Path $ds "native\bin\x64\")
        Copy-Item (Join-Path $tmpDir "native\lib\x64\dstorage.lib")   (Join-Path $ds "native\lib\x64\")
        Copy-Item (Join-Path $tmpDir "LICENSE.txt")                   (Join-Path $ds "LICENSE.txt")
        if (Test-Path (Join-Path $tmpDir "NOTICES.txt")) {
            Copy-Item (Join-Path $tmpDir "NOTICES.txt") (Join-Path $ds "NOTICES.txt")
        }
    }
}
if (-not (Test-Path (Join-Path $ds "native\include\dstorage.h"))) {
    throw "DirectStorage header still missing after fetch: $ds"
}

# --- Visual Studio toolchain (version-agnostic) ------------------------------
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    # Some shells (e.g. Git Bash) do not forward the "ProgramFiles(x86)" env
    # var to child processes; fall back to the canonical absolute location.
    $vswhere = "${env:SystemDrive}\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
}
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found; install Visual Studio Build Tools" }
$vs = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
if (-not $vs) { throw "Visual Studio with C++ MSBuild tools not found" }
$vsVer = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationVersion
$vsMajor = [int]($vsVer.Split('.')[0])
switch ($vsMajor) {
    18 { $generator = "Visual Studio 18 2026" }
    17 { $generator = "Visual Studio 17 2022" }
    16 { $generator = "Visual Studio 16 2019" }
    default { throw "Unsupported Visual Studio major version: $vsMajor ($vsVer)" }
}
Write-Host "Using $generator (VS $vsVer at $vs)"
$vsDev = Join-Path $vs "Common7\Tools\VsDevCmd.bat"

$cmd = @"
call "$vsDev" -arch=amd64 -host_arch=amd64 >nul
cmake -S . -B build -G "$generator" -A x64
if errorlevel 1 exit /b 1
cmake --build build --config Release --parallel
if errorlevel 1 exit /b 1
"@
$tmp = Join-Path $env:TEMP "ngdc_build_12.cmd"
Set-Content -Path $tmp -Value $cmd -Encoding ASCII
& cmd.exe /c $tmp
if ($LASTEXITCODE -ne 0) { throw "Build failed: $LASTEXITCODE" }

$dll = Join-Path $here "package\SKSE\Plugins\NextGenDiskCache.dll"
if (-not (Test-Path $dll)) { throw "DLL missing after build: $dll" }
Get-Item $dll | Format-List FullName, Length, LastWriteTime
Write-Host "BUILD OK: $dll"
