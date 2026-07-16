# Build NextGenDiskCache.dll → package/SKSE/Plugins
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $here

$vsDev = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat"
if (-not (Test-Path $vsDev)) {
    throw "VsDevCmd.bat not found: $vsDev"
}

$cmd = @"
call "$vsDev" -arch=amd64 -host_arch=amd64 >nul
cmake -B build -G "Visual Studio 18 2026" -A x64
if errorlevel 1 exit /b 1
cmake --build build --config Release -v
if errorlevel 1 exit /b 1
"@

$tmp = Join-Path $env:TEMP "ngdc_build.cmd"
Set-Content -Path $tmp -Value $cmd -Encoding ASCII
& cmd.exe /c $tmp
if ($LASTEXITCODE -ne 0) { throw "Build failed: $LASTEXITCODE" }

$dll = Join-Path $here "package\SKSE\Plugins\NextGenDiskCache.dll"
if (-not (Test-Path $dll)) { throw "DLL missing after build: $dll" }
Get-Item $dll | Format-List FullName, Length, LastWriteTime
Write-Host "BUILD OK"
