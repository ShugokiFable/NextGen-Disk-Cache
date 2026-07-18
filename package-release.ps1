param(
    [string]$Version = "1.2.0",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

if (-not $SkipBuild) {
    & (Join-Path $root "build.ps1")
    if ($LASTEXITCODE -ne 0) { throw "build.ps1 failed with exit code $LASTEXITCODE" }
}

$dll = Join-Path $root "package\SKSE\Plugins\NextGenDiskCache.dll"
if (-not (Test-Path $dll)) { throw "Missing release DLL: $dll" }

$distRoot = Join-Path $root "dist"
$stage = Join-Path $distRoot "NextGenDiskCache-$Version"
$zip = Join-Path $distRoot "NextGenDiskCache-$Version-FOMOD.zip"
$shaFile = Join-Path $distRoot "NextGenDiskCache-$Version-SHA256.txt"

Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item $zip -Force -ErrorAction SilentlyContinue
New-Item (Join-Path $stage "Core\SKSE\Plugins") -ItemType Directory -Force | Out-Null
New-Item (Join-Path $stage "Profiles\HighEnd") -ItemType Directory -Force | Out-Null
New-Item (Join-Path $stage "Profiles\Balanced") -ItemType Directory -Force | Out-Null
New-Item (Join-Path $stage "Optional\DirectStorage") -ItemType Directory -Force | Out-Null

Copy-Item $dll (Join-Path $stage "Core\SKSE\Plugins\")
Copy-Item "profiles\HighEnd\NextGenDiskCache.ini" (Join-Path $stage "Profiles\HighEnd\")
Copy-Item "profiles\Balanced\NextGenDiskCache.ini" (Join-Path $stage "Profiles\Balanced\")
Copy-Item "fomod" (Join-Path $stage "fomod") -Recurse

$rootDocs = @(
    "README.md",
    "CHANGELOG.txt",
    "LICENSE.txt",
    "LICENSE.Archost-DiskCacheEnabler.txt",
    "LICENSE.detours.txt",
    "LICENSE.SKSE64.txt"
)
foreach ($doc in $rootDocs) {
    if (-not (Test-Path $doc)) { throw "Missing release document: $doc" }
    Copy-Item $doc $stage
}

$dsBin = Join-Path $root "deps\directstorage\native\bin\x64"
$dsLicense = Join-Path $root "deps\directstorage\LICENSE.txt"
if (Test-Path $dsBin) {
    $runtimeDlls = Get-ChildItem $dsBin -Filter "dstorage*.dll" -File
    if ($runtimeDlls.Count -lt 2) {
        throw "DirectStorage package is incomplete; expected dstorage.dll and dstoragecore.dll"
    }
    $runtimeDlls | Copy-Item -Destination (Join-Path $stage "Optional\DirectStorage\")
    if (-not (Test-Path $dsLicense)) { throw "DirectStorage redistributable license missing" }
    Copy-Item $dsLicense (Join-Path $stage "LICENSE.DirectStorage.txt")
} else {
    throw "DirectStorage SDK/runtime not installed at $dsBin"
}

# FOMOD and payload gates.
[xml]$moduleConfig = Get-Content (Join-Path $stage "fomod\ModuleConfig.xml") -Raw
if ($moduleConfig.config.moduleName -ne "NextGen Disk Cache $Version") {
    throw "FOMOD module version mismatch"
}

$forbidden = Get-ChildItem $stage -Recurse -File | Where-Object {
    $_.Extension -in @(".pdb", ".lib", ".exp", ".ilk", ".obj", ".iobj", ".ipdb")
}
if ($forbidden) {
    throw "Forbidden build artifacts in public stage: $($forbidden.FullName -join ', ')"
}

$coreDlls = @(Get-ChildItem (Join-Path $stage "Core\SKSE\Plugins") -Filter "*.dll" -File)
if ($coreDlls.Count -ne 1 -or $coreDlls[0].Name -ne "NextGenDiskCache.dll") {
    throw "Core payload must contain exactly NextGenDiskCache.dll"
}

Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip -CompressionLevel Optimal
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant()
"$hash  $(Split-Path $zip -Leaf)" | Set-Content $shaFile -Encoding ASCII

Write-Host "RELEASE PACKAGE OK"
Write-Host "ZIP: $zip"
Write-Host "SHA256: $hash"
