<#
    build.ps1 - one-command source build of SmackMyRezUp (Windows x64 / D3D12).

    Run it and it will:
      1. locate Visual Studio 2022 (C++), CMake and git
      2. clone/update NVIDIA's public DLSS SDK           -> external/DLSS
      3. make sure the NR runtime (nvngx_dlssnr.dll) is present in payload/
         (this one is NOT public - you must supply it, see below)
      4. download FFmpeg (or reuse one already on the PC)  -> payload/
      5. configure + build Release
      6. print the exe path and its SHA-256

    Everything the exe needs at runtime is embedded into the exe itself, so the
    finished build\Release\SmackMyRezUp.exe is self-contained.

    The NR runtime:
      nvngx_dlssnr.dll is the neural-rendering snippet and is not redistributable
      through the public SDK. Put it next to this script or inside payload\ before
      running, or pass its path:
          .\build.ps1 -DlssnrDll C:\path\to\nvngx_dlssnr.dll

    Requirements on the machine:
      - Visual Studio 2022 with "Desktop development with C++" (MSVC + Windows SDK)
      - CMake 3.24+ (the VS component "C++ CMake tools" provides one)
      - Git for Windows
      - An NVIDIA RTX GPU + a current driver (NR needs a recent driver)
#>
[CmdletBinding()]
param(
    [string]$DlssnrDll = "",          # path to nvngx_dlssnr.dll if not already in payload\
    [switch]$Clean                    # wipe build\ before configuring
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
Set-Location $root

function Fail($msg) { Write-Host "`n[ERROR] $msg" -ForegroundColor Red; exit 1 }
function Info($msg) { Write-Host "[*] $msg" -ForegroundColor Cyan }
function Ok($msg)   { Write-Host "[OK] $msg" -ForegroundColor Green }
# Windows PowerShell 5.1 has no ?. operator, so resolve commands the safe way.
function Which($name) { $c = Get-Command $name -ErrorAction SilentlyContinue; if ($c) { $c.Source } else { $null } }

# --- 1. tools -------------------------------------------------------------
Info "Locating build tools..."

$git = Which git.exe
if (-not $git) { Fail "git.exe not found. Install Git for Windows: https://git-scm.com/download/win" }

# -products * so BuildTools (not just Community/Pro/Enterprise) is found too.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = $null
if (Test-Path $vswhere) {
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null | Select-Object -First 1
}
# Fallback: probe the standard install locations for an MSVC toolset directory.
if (-not $vsPath) {
    foreach ($ed in "Community","Professional","Enterprise","BuildTools") {
        $p = "${env:ProgramFiles}\Microsoft Visual Studio\2022\$ed"
        if (Test-Path (Join-Path $p "VC\Tools\MSVC")) { $vsPath = $p; break }
    }
}
if (-not $vsPath) { Fail "Visual Studio 2022 with the C++ toolset was not found. In the VS Installer add 'Desktop development with C++' (or install Build Tools for VS 2022)." }
Ok "Visual Studio: $vsPath"

# Prefer a CMake on PATH; otherwise use the one bundled with VS.
$cmake = Which cmake.exe
if (-not $cmake) {
    $vsCmake = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if (Test-Path $vsCmake) { $cmake = $vsCmake }
}
if (-not $cmake) { Fail "CMake not found. Add 'C++ CMake tools for Windows' in the VS Installer, or install CMake 3.24+." }
Ok "CMake: $cmake"

$payload = Join-Path $root "payload"
New-Item -ItemType Directory -Force $payload | Out-Null

# --- 2. NVIDIA DLSS SDK (public) -----------------------------------------
$sdk = Join-Path $root "external\DLSS"
if (Test-Path (Join-Path $sdk "include\nvsdk_ngx.h")) {
    Info "DLSS SDK present; updating..."
    & $git -C $sdk pull --ff-only 2>$null | Out-Null
} else {
    Info "Cloning NVIDIA DLSS SDK (public)..."
    New-Item -ItemType Directory -Force (Join-Path $root "external") | Out-Null
    & $git clone --depth 1 https://github.com/NVIDIA/DLSS.git $sdk
    if ($LASTEXITCODE -ne 0) { Fail "Failed to clone NVIDIA/DLSS." }
}
Ok "DLSS SDK ready"

# --- 3. NR runtime (user-supplied) ---------------------------------------
# Search order: explicit param -> nr-runtime\ (the committed drop folder)
# -> repo root -> already in payload\.
$nrDst = Join-Path $payload "nvngx_dlssnr.dll"
$nrDrop = Join-Path $root "nr-runtime\nvngx_dlssnr.dll"
if ($DlssnrDll -and (Test-Path $DlssnrDll)) {
    Copy-Item $DlssnrDll $nrDst -Force
    Ok "NR runtime staged from -DlssnrDll"
} elseif (Test-Path $nrDrop) {
    Copy-Item $nrDrop $nrDst -Force
    Ok "NR runtime staged from nr-runtime\"
} elseif (Test-Path (Join-Path $root "nvngx_dlssnr.dll")) {
    Copy-Item (Join-Path $root "nvngx_dlssnr.dll") $nrDst -Force
    Ok "NR runtime staged from repo root"
} elseif (Test-Path $nrDst) {
    Ok "NR runtime already in payload\"
} else {
    Fail @"
nvngx_dlssnr.dll (the neural-rendering runtime) was not found.
It is not part of the public SDK, so it cannot be downloaded here.
Drop it in nr-runtime\ (see nr-runtime\README.md), or pass:
    .\build.ps1 -DlssnrDll C:\path\to\nvngx_dlssnr.dll
"@
}

# --- 4. FFmpeg ------------------------------------------------------------
$ff = Join-Path $payload "ffmpeg.exe"
$fp = Join-Path $payload "ffprobe.exe"
if ((Test-Path $ff) -and (Test-Path $fp)) {
    Ok "FFmpeg already staged"
} else {
    $sysFf = Which ffmpeg.exe
    $sysFp = Which ffprobe.exe
    if ($sysFf -and $sysFp) {
        Copy-Item $sysFf $ff -Force; Copy-Item $sysFp $fp -Force
        Ok "FFmpeg reused from PATH"
    } else {
        Info "Downloading FFmpeg (essentials build)..."
        $zip = Join-Path $root "external\ffmpeg.zip"
        $tmp = Join-Path $root "external\ffmpeg_unpack"
        $url = "https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip"
        New-Item -ItemType Directory -Force (Join-Path $root "external") | Out-Null
        if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }
        curl.exe -L --fail --retry 3 --progress-bar -o $zip $url
        if ($LASTEXITCODE -ne 0) { Fail "FFmpeg download failed." }
        Expand-Archive -LiteralPath $zip -DestinationPath $tmp -Force
        $ffFound = Get-ChildItem $tmp -Recurse -Filter ffmpeg.exe  | Select-Object -First 1
        $fpFound = Get-ChildItem $tmp -Recurse -Filter ffprobe.exe | Select-Object -First 1
        if (-not $ffFound -or -not $fpFound) { Fail "ffmpeg/ffprobe not found inside the downloaded archive." }
        Copy-Item $ffFound.FullName $ff -Force
        Copy-Item $fpFound.FullName $fp -Force
        Remove-Item $tmp -Recurse -Force
        Ok "FFmpeg downloaded and staged"
    }
}

# --- 5. configure + build -------------------------------------------------
$build = Join-Path $root "build"
if ($Clean -and (Test-Path $build)) { Info "Cleaning build\..."; Remove-Item $build -Recurse -Force }

Info "Configuring (Visual Studio 17 2022, x64)..."
& $cmake -S . -B build -G "Visual Studio 17 2022" -A x64
if ($LASTEXITCODE -ne 0) { Fail "CMake configure failed." }

Info "Building Release..."
& $cmake --build build --config Release --parallel
if ($LASTEXITCODE -ne 0) { Fail "Build failed. Send the compiler output above." }

$exe = Join-Path $build "Release\SmackMyRezUp.exe"
if (-not (Test-Path $exe)) { Fail "Build reported success but the exe is missing." }

$hash = (Get-FileHash $exe -Algorithm SHA256).Hash
$version = (Get-Content (Join-Path $root "VERSION") -TotalCount 1).Trim()
Write-Host ""
Ok "SmackMyRezUp $version build complete."
Write-Host "    exe    : $exe"
Write-Host "    size   : $([Math]::Round((Get-Item $exe).Length/1MB,1)) MB (runtime is embedded)"
Write-Host "    sha256 : $hash"
Write-Host ""
Write-Host "Run it from anywhere; on first launch it extracts its runtime beside itself." -ForegroundColor Gray
