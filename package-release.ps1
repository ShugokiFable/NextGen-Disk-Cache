param(
    [string]$Version = "2.0.0",
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
New-Item (Join-Path $stage "Profiles\SafeDefault") -ItemType Directory -Force | Out-Null
New-Item (Join-Path $stage "Profiles\Minimal") -ItemType Directory -Force | Out-Null
New-Item (Join-Path $stage "Profiles\ExperimentalWarmCache") -ItemType Directory -Force | Out-Null

Copy-Item $dll (Join-Path $stage "Core\SKSE\Plugins\")
Copy-Item "profiles\SafeDefault\NextGenDiskCache.ini" (Join-Path $stage "Profiles\SafeDefault\")
Copy-Item "profiles\Minimal\NextGenDiskCache.ini" (Join-Path $stage "Profiles\Minimal\")
Copy-Item "profiles\ExperimentalWarmCache\NextGenDiskCache.ini" (Join-Path $stage "Profiles\ExperimentalWarmCache\")
Copy-Item "fomod" (Join-Path $stage "fomod") -Recurse

$rootDocs = @(
    "README.md",
    "CHANGELOG.txt",
    "PACKAGE-NOTICE.txt",
    "LICENSE.txt",
    "LICENSE.Archost-DiskCacheEnabler.txt",
    "LICENSE.detours.txt",
    "LICENSE.SKSE64.txt"
)
foreach ($doc in $rootDocs) {
    if (-not (Test-Path $doc)) { throw "Missing release document: $doc" }
    Copy-Item $doc $stage
}

# DirectStorage runtime is intentionally not shipped in the public package.
# Backend code may still compile for development; all shipped profiles leave it off.

# FOMOD and payload gates.
[xml]$moduleConfig = Get-Content (Join-Path $stage "fomod\ModuleConfig.xml") -Raw
if ($moduleConfig.config.moduleName -ne "NextGen Disk Cache $Version") {
    throw "FOMOD module version mismatch"
}
if ($moduleConfig.config.installSteps.installStep.Count -ne 1 -and
    @($moduleConfig.config.installSteps.installStep).Count -ne 1) {
    # Single "Choose a build" step only (no DirectStorage step).
    $stepCount = @($moduleConfig.config.installSteps.installStep).Count
    if ($stepCount -ne 1) {
        throw "FOMOD must contain exactly one install step (profiles only); found $stepCount"
    }
}

$dsFolders = Get-ChildItem $stage -Recurse -Directory -Filter "DirectStorage" -ErrorAction SilentlyContinue
if ($dsFolders) {
    throw "Public package must not contain DirectStorage runtime folders: $($dsFolders.FullName -join ', ')"
}
$dsDlls = Get-ChildItem $stage -Recurse -File -Filter "dstorage*.dll" -ErrorAction SilentlyContinue
if ($dsDlls) {
    throw "Public package must not contain DirectStorage DLLs: $($dsDlls.FullName -join ', ')"
}
$dsLicense = Join-Path $stage "LICENSE.DirectStorage.txt"
if (Test-Path $dsLicense) {
    throw "Public package must not contain LICENSE.DirectStorage.txt when runtime is omitted"
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
Write-Host "NOTE: DirectStorage runtime intentionally omitted from public package"
