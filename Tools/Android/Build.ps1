# Builds the engine and a game (the Dice game for now) for Android arm64.
#   Tools/Android/Build.ps1 [-Config Debug|Release] [-Configure]
# Uses Visual Studio's CMake, Ninja and vcpkg, and the Android NDK from the SDK.
param(
    [ValidateSet('Debug', 'Release')] [string] $Config = 'Debug',
    [switch] $Configure
)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path "$PSScriptRoot/../.."

$vs = & "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -prerelease -property installationPath
$cmakeDir = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake'
$cmake = Join-Path $cmakeDir 'CMake/bin/cmake.exe'
$env:PATH = (Join-Path $cmakeDir 'Ninja') + ';' + $env:PATH
if (-not $env:VCPKG_ROOT) { $env:VCPKG_ROOT = Join-Path $vs 'VC/vcpkg' }

$sdk = if ($env:ANDROID_HOME) { $env:ANDROID_HOME } else { Join-Path $env:LOCALAPPDATA 'Android/Sdk' }
if (-not $env:ANDROID_NDK_HOME) {
    $ndk = Get-ChildItem (Join-Path $sdk 'ndk') -Directory | Sort-Object { [version]$_.Name } | Select-Object -Last 1
    if (-not $ndk) { throw "No NDK under $sdk/ndk. Install one with the Android Studio SDK Manager." }
    $env:ANDROID_NDK_HOME = $ndk.FullName
}
Write-Host "NDK: $env:ANDROID_NDK_HOME"
Write-Host "vcpkg: $env:VCPKG_ROOT"

$build = Join-Path $root 'out/build/android-arm64'
Push-Location $root
try {
    if ($Configure -or -not (Test-Path (Join-Path $build 'CMakeCache.txt'))) {
        & $cmake --preset android-arm64
        if ($LASTEXITCODE) { throw "configure failed ($LASTEXITCODE)" }
    }
    & $cmake --build $build --config $Config
    if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }
} finally { Pop-Location }
