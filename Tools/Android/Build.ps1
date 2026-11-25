# Builds the engine and the active game (Dice for now) for Android: libgame.so per ABI.
#   Tools/Android/Build.ps1 [-Config Debug|Release] [-Abi arm64-v8a,x86_64] [-Configure]
# arm64-v8a is for phones, x86_64 for emulators. Uses Visual Studio's CMake, Ninja and vcpkg,
# and the newest NDK in the Android SDK. Output: out/build/<preset>/CherryLaunch/<Config>/libgame.so
param(
    [ValidateSet('Debug', 'Release')] [string] $Config = 'Debug',
    [string[]] $Abi = @('arm64-v8a'),
    [switch] $Configure
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/Common.ps1"
$tools = Get-CherryAndroidTools
$Abi = @($Abi | ForEach-Object { $_ -split ',' } | Where-Object { $_ })   # -File passes "a,b" as one string

foreach ($a in $Abi) {
    $preset = Get-CherryAndroidPreset $a
    $build = Join-Path $tools.Root "out/build/$preset"
    Push-Location $tools.Root
    try {
        if ($Configure -or -not (Test-Path (Join-Path $build 'CMakeCache.txt'))) {
            & $tools.CMake --preset $preset
            if ($LASTEXITCODE) { throw "configure ($preset) failed ($LASTEXITCODE)" }
        }
        & $tools.CMake --build $build --config $Config
        if ($LASTEXITCODE) { throw "build ($preset, $Config) failed ($LASTEXITCODE)" }
    } finally { Pop-Location }
}
