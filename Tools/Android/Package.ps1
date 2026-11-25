# Exports a CherryEngine project as an Android APK:
#   Tools/Android/Package.ps1 -Project Dice/Dice.cherryproject [-Config Debug|Release]
#       [-Abi arm64-v8a,x86_64] [-Install] [-Run] [-SkipNative]
# 1. builds libgame.so for each ABI (Build.ps1; only games that build for Android: $AndroidGames),
# 2. stages the project file and its Content/ as the APK's assets, with files.txt (a build stamp,
#    then every path) for the app to unpack on first start (CherryLaunch/AndroidMain.cpp), and the
#    launcher icon,
# 3. runs Gradle on the template in Android/ with the project's "android" settings,
# 4. copies the APK to out/android/<Module>-<Config>.apk, and optionally installs and starts it.
#
# The project's "android" section: packageId, label, versionCode, versionName, orientation and
# icon (a PNG, relative to the project file; 192x192 or so).
# Release signing: set CHERRY_ANDROID_KEYSTORE, CHERRY_ANDROID_KEYSTORE_PASSWORD,
# CHERRY_ANDROID_KEY_ALIAS and CHERRY_ANDROID_KEY_PASSWORD (never keep them in the repository);
# without them a release APK is signed with the debug key (fine for installing by hand).
param(
    [Parameter(Mandatory = $true)] [string] $Project,
    [ValidateSet('Debug', 'Release')] [string] $Config = 'Debug',
    [string[]] $Abi = @('arm64-v8a'),
    [switch] $Install,
    [switch] $Run,
    [switch] $SkipNative
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/Common.ps1"
$tools = Get-CherryAndroidTools
$Abi = @($Abi | ForEach-Object { $_ -split ',' } | Where-Object { $_ })

# The games whose code builds for Android (the presets' CHERRY_ACTIVE_GAME_TARGET).
$AndroidGames = @('DiceGame')

$projectFile = (Resolve-Path $Project).Path
$projectDir = Split-Path $projectFile
$desc = Get-Content $projectFile -Raw | ConvertFrom-Json
$module = $desc.module
if ($AndroidGames -notcontains $module) {
    throw "$module doesn't build for Android yet (Android games: $($AndroidGames -join ', ')). See Dice/Docs/Android.md."
}
$android = if ($desc.PSObject.Properties['android']) { $desc.android } else { [pscustomobject]@{} }
function Setting($name, $fallback) { if ($android.PSObject.Properties[$name]) { $android.$name } else { $fallback } }
$appId = Setting 'packageId' ('com.cherryengine.' + ($module.ToLower() -replace '[^a-z0-9]', ''))
$label = Setting 'label' $desc.name
$versionCode = Setting 'versionCode' 1
$versionName = Setting 'versionName' '1.0'
$orientation = Setting 'orientation' 'landscape'
$icon = Setting 'icon' ''

# 1. Native libraries.
if (-not $SkipNative) {
    & "$PSScriptRoot/Build.ps1" -Config $Config -Abi ($Abi -join ',')
    if (-not $?) { throw 'native build failed' }
}

# 2. Staging.
$stage = Join-Path $tools.Root "out/android/$module"
$jni = Join-Path $stage 'jniLibs'
$assets = Join-Path $stage 'assets'
$res = Join-Path $stage 'res'
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
foreach ($a in $Abi) {
    $lib = Join-Path $tools.Root "out/build/$(Get-CherryAndroidPreset $a)/CherryLaunch/$Config/libgame.so"
    if (-not (Test-Path $lib)) { throw "Missing $lib (build it, or drop -SkipNative)" }
    New-Item -ItemType Directory -Force (Join-Path $jni $a) | Out-Null
    Copy-Item $lib (Join-Path $jni $a)
}
New-Item -ItemType Directory -Force $assets | Out-Null
Copy-Item $projectFile $assets
$contentName = if ($desc.PSObject.Properties['content']) { $desc.content } else { 'Content' }
Copy-Item (Join-Path $projectDir $contentName) (Join-Path $assets $contentName) -Recurse
$files = Get-ChildItem $assets -Recurse -File | ForEach-Object { $_.FullName.Substring($assets.Length + 1).Replace('\', '/') }
$stamp = "$versionName-$(Get-Date -Format 'yyyyMMddHHmmss')"
Set-Content -Path (Join-Path $assets 'files.txt') -Value (@($stamp) + $files) -Encoding ascii
$resArgs = @()
if ($icon) {
    $iconPath = Join-Path $projectDir $icon
    if (-not (Test-Path $iconPath)) { throw "The project's android.icon $iconPath doesn't exist" }
    New-Item -ItemType Directory -Force (Join-Path $res 'mipmap') | Out-Null
    Copy-Item $iconPath (Join-Path $res 'mipmap/ic_launcher.png')
    $resArgs = @("-PcherryRes=$res")
}
Write-Host "Staged $($files.Count) files and $($Abi -join ', ') in $stage"

# 3. Gradle.
if ($Config -eq 'Release' -and -not $env:CHERRY_ANDROID_KEYSTORE) {
    Write-Warning 'No CHERRY_ANDROID_KEYSTORE: the release APK is signed with the debug key (installable by hand, not for a store).'
}
$ndkVersion = Split-Path $tools.Ndk -Leaf
$task = if ($Config -eq 'Release') { 'assembleRelease' } else { 'assembleDebug' }
Push-Location (Join-Path $tools.Root 'Android')
try {
    & ./gradlew.bat $task --console=plain -q `
        "-PcherryAppId=$appId" "-PcherryLabel=$label" "-PcherryVersionCode=$versionCode" "-PcherryVersionName=$versionName" `
        "-PcherryOrientation=$orientation" "-PcherryJniLibs=$jni" "-PcherryAssets=$assets" "-PcherryNdkVersion=$ndkVersion" @resArgs
    if ($LASTEXITCODE) { throw "Gradle failed ($LASTEXITCODE)" }
} finally { Pop-Location }
$built = Join-Path $tools.Root "Android/app/build/outputs/apk/$($Config.ToLower())/app-$($Config.ToLower()).apk"
$apk = Join-Path $tools.Root "out/android/$module-$Config.apk"
Copy-Item $built $apk -Force
Write-Host "APK: $apk ($([math]::Round((Get-Item $apk).Length / 1MB, 1)) MB, $appId $versionName)"

# 4. Device.
if ($Install -or $Run) {
    & $tools.Adb install -r $apk
    if ($LASTEXITCODE) { throw "adb install failed ($LASTEXITCODE)" }
}
if ($Run) {
    & $tools.Adb shell am start -n "$appId/android.app.NativeActivity"
}
