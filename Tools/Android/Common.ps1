# Shared by the Android scripts: finds the tools and sets the environment the presets read.

function Get-CherryAndroidTools {
    $root = (Resolve-Path "$PSScriptRoot/../..").Path
    $vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
    $vs = & $vswhere -latest -prerelease -property installationPath
    if (-not $vs) { throw "Visual Studio not found (vswhere)." }
    $cmakeDir = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake'
    $env:PATH = (Join-Path $cmakeDir 'Ninja') + ';' + $env:PATH
    if (-not $env:VCPKG_ROOT) { $env:VCPKG_ROOT = Join-Path $vs 'VC/vcpkg' }

    $sdk = if ($env:ANDROID_HOME) { $env:ANDROID_HOME } elseif ($env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT } else { Join-Path $env:LOCALAPPDATA 'Android/Sdk' }
    if (-not (Test-Path $sdk)) { throw "No Android SDK at $sdk. Install Android Studio or set ANDROID_HOME." }
    $env:ANDROID_HOME = $sdk
    if (-not $env:ANDROID_NDK_HOME) {
        $ndk = Get-ChildItem (Join-Path $sdk 'ndk') -Directory -ErrorAction SilentlyContinue | Sort-Object { [version]$_.Name } | Select-Object -Last 1
        if (-not $ndk) { throw "No NDK under $sdk/ndk. Install one with the Android Studio SDK Manager." }
        $env:ANDROID_NDK_HOME = $ndk.FullName
    }
    [pscustomobject]@{
        Root  = $root
        CMake = Join-Path $cmakeDir 'CMake/bin/cmake.exe'
        Sdk   = $sdk
        Ndk   = $env:ANDROID_NDK_HOME
        Adb   = Join-Path $sdk 'platform-tools/adb.exe'
    }
}

function Get-CherryAndroidPreset([string] $abi) {
    switch ($abi) {
        'arm64-v8a' { 'android-arm64' }
        'x86_64' { 'android-x64' }
        default { throw "Unknown ABI $abi" }
    }
}
