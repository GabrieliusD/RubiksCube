# Screenshot.ps1: runs UNDERTIME at a chosen spot and time, captures the game window to a PNG,
# closes the game and prints the interesting log lines. For checking visuals without anyone
# at the keyboard (see .claude/skills/undertime-screenshots/SKILL.md).
#
#   powershell -File Undertime\Tools\Screenshot.ps1 -View "24.5 1.6 14 180 0" -Out shots\mouth.png
#   ... -Showcase -View "30 1 14 90 8" -Out shots\ores.png          (the showcase hall)
#   ... -Time "17 3" -Weather "1 1 1" -Seed 12345 -Wait 12
#
# -View    "x y z yaw pitch [hold]": feet position (m), yaw/pitch in degrees (yaw 0 faces +z,
#          into the mountain; 90 faces +x; pitch > 0 looks down). "hold": stay put in mid-air.
# -Out     PNG path (relative paths are relative to the current directory).
# -Wait    seconds after the window appears before the capture (the valley takes ~2 s to
#          build; the eyes take a few seconds to adjust in the cave).
# -Time    "hour [years]" (UNDERTIME_DEV_TIME).    -Seed N (UNDERTIME_DEV_SEED).
# -Weather "overcast rain wet" (0..1 each).        -Sun "x y z" (the sunlight's direction).
# -Showcase  build the showcase cave (UNDERTIME_DEV_SHOWCASE=1).
# -Talk    "id [continues]": stand in front of that person and talk to them (UNDERTIME_DEV_TALK).
# -Missions "ids... [journal|board]": take on missions and open the journal or board (UNDERTIME_DEV_MISSIONS).
# -Config  Release (default) or Debug.             -Log  where the game's output goes.
param(
	[Parameter(Mandatory = $true)][string]$View,
	[Parameter(Mandatory = $true)][string]$Out,
	[int]$Wait = 10,
	[string]$Time = "",
	[string]$Seed = "",
	[string]$Weather = "",
	[string]$Sun = "",
	[switch]$Showcase,
	[string]$Talk = "",
	[string]$Missions = "",
	[string]$Config = "Release",
	[string]$Log = ""
)
$ErrorActionPreference = "Stop"

$repo = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$bin = Join-Path $repo "out\build\x64-debug\bin\$Config"
$project = Join-Path $repo "Undertime\Undertime.cherryproject"
if (-not (Test-Path "$bin\CherryLaunch.exe")) { throw "No $bin\CherryLaunch.exe: build the $Config configuration first." }
$outPath = if ([System.IO.Path]::IsPathRooted($Out)) { $Out } else { [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $Out)) }
New-Item -ItemType Directory -Force (Split-Path $outPath) | Out-Null
if (-not $Log) { $Log = [System.IO.Path]::ChangeExtension($outPath, ".log") }

if (-not ("UndertimeShot" -as [type])) {
Add-Type @"
using System; using System.Runtime.InteropServices;
public class UndertimeShot {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int hh, bool repaint);
  public struct RECT { public int L, T, R, B; }
}
"@
}
Add-Type -AssemblyName System.Drawing
# Without this, a scaled display (125%, 150%) gives a client rectangle in the wrong units.
[UndertimeShot]::SetProcessDPIAware() | Out-Null

# The game reads these at start-up; set them only for this launch.
$settings = @{
	UNDERTIME_DEV_VIEW = $View
	UNDERTIME_DEV_TIME = $Time
	UNDERTIME_DEV_SEED = $Seed
	UNDERTIME_DEV_WEATHER = $Weather
	UNDERTIME_DEV_SUN = $Sun
	UNDERTIME_DEV_SHOWCASE = $(if ($Showcase) { "1" } else { "" })
	UNDERTIME_DEV_TALK = $Talk
	UNDERTIME_DEV_MISSIONS = $Missions
}
foreach ($k in $settings.Keys) { if ($settings[$k]) { Set-Item "Env:$k" $settings[$k] } else { Remove-Item "Env:$k" -ErrorAction SilentlyContinue } }
try {
	$p = Start-Process -FilePath "$bin\CherryLaunch.exe" -ArgumentList "`"$project`" --play" -WorkingDirectory $bin `
		-RedirectStandardOutput $Log -RedirectStandardError "$Log.err" -PassThru
} finally {
	foreach ($k in $settings.Keys) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
}

$h = [IntPtr]::Zero
for ($i = 0; $i -lt 40 -and $h -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 500; $p.Refresh(); $h = $p.MainWindowHandle }
if ($h -eq [IntPtr]::Zero) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; throw "The game window never appeared (see $Log)." }
[UndertimeShot]::MoveWindow($h, 20, 20, 1300, 780, $true) | Out-Null
Start-Sleep -Seconds $Wait

# PrintWindow (flag 3: the full client area, including DirectX content) captures the window
# itself, so other windows on top don't end up in the picture.
$r = New-Object UndertimeShot+RECT; [UndertimeShot]::GetClientRect($h, [ref]$r) | Out-Null
$bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
$g = [System.Drawing.Graphics]::FromImage($bmp); $hdc = $g.GetHdc()
[UndertimeShot]::PrintWindow($h, $hdc, 3) | Out-Null
$g.ReleaseHdc($hdc); $bmp.Save($outPath); $g.Dispose(); $bmp.Dispose()
Stop-Process -Id $p.Id -Force

Write-Output "Saved $outPath"
Get-Content $Log, "$Log.err" -ErrorAction SilentlyContinue |
	Select-String -Pattern "UNDERTIME|error|Error|fail|Fail|warn" | Select-Object -First 25 | ForEach-Object { $_.Line }
