# Screenshot.ps1: runs the dice game, captures the window to a PNG, closes it and prints the
# DICE log lines. For checking the port's visuals without anyone at the keyboard.
#
#   powershell -File Dice\Tools\Screenshot.ps1 -Out shots\board.png -Density 100 -Face 6
#
# -Face 1..6 deals every die with that face up (DICE_DEV_FACE). -Density percent (DICE_DEV_DENSITY).
# -Pointer "px py [px2 py2 seconds]" a pretend mouse in view pixels (DICE_DEV_POINTER).
# -Storm P (DICE_DEV_STORM, % of dice launched per second). -Profile (DICE_DEV_PROFILE). -Preset name (DICE_DEV_PRESET, Content/Presets/<name>.json). -Mode 0|1|2 nudge, bump, stack (DICE_DEV_MODE). -Layout 0|1|2 square, hex, scatter. -Shape 0..3 full, circle, ring, islands. -Session S start a session of S seconds (DICE_DEV_SESSION). -Board N board size (DICE_DEV_BOARD). -Seed N repeatable deal (DICE_DEV_SEED). -Wait seconds before the capture. -Config Debug (default) or Release.
param(
	[Parameter(Mandatory = $true)][string]$Out,
	[int]$Wait = 5,
	[string]$Face = "",
	[string]$Density = "",
	[string]$Seed = "",
	[string]$Board = "",
	[string]$Pointer = "",
	[string]$Session = "",
	[string]$Mode = "",
	[string]$Layout = "",
	[string]$Preset = "",
	[string]$Storm = "",
	[switch]$Profile,
	[string]$Shape = "",
	[string]$Config = "Debug",
	[string]$Log = ""
)
$ErrorActionPreference = "Stop"

$repo = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$bin = Join-Path $repo "out\build\x64-debug\bin\$Config"
$project = Join-Path $repo "Dice\Dice.cherryproject"
if (-not (Test-Path "$bin\CherryLaunch.exe")) { throw "No $bin\CherryLaunch.exe: build the $Config configuration first." }
$outPath = if ([System.IO.Path]::IsPathRooted($Out)) { $Out } else { [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $Out)) }
New-Item -ItemType Directory -Force (Split-Path $outPath) | Out-Null
if (-not $Log) { $Log = [System.IO.Path]::ChangeExtension($outPath, ".log") }

if (-not ("DiceShot" -as [type])) {
Add-Type @"
using System; using System.Runtime.InteropServices;
public class DiceShot {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int hh, bool repaint);
  public struct RECT { public int L, T, R, B; }
}
"@
}
Add-Type -AssemblyName System.Drawing
[DiceShot]::SetProcessDPIAware() | Out-Null

$settings = @{ DICE_DEV_FACE = $Face; DICE_DEV_DENSITY = $Density; DICE_DEV_SEED = $Seed; DICE_DEV_BOARD = $Board; DICE_DEV_POINTER = $Pointer; DICE_DEV_SESSION = $Session; DICE_DEV_MODE = $Mode; DICE_DEV_LAYOUT = $Layout; DICE_DEV_SHAPE = $Shape; DICE_DEV_PRESET = $Preset; DICE_DEV_STORM = $Storm; DICE_DEV_PROFILE = $(if ($Profile) { "1" } else { "" }) }
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
[DiceShot]::MoveWindow($h, 20, 20, 1300, 780, $true) | Out-Null
Start-Sleep -Seconds $Wait

$r = New-Object DiceShot+RECT; [DiceShot]::GetClientRect($h, [ref]$r) | Out-Null
$bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
$g = [System.Drawing.Graphics]::FromImage($bmp); $hdc = $g.GetHdc()
[DiceShot]::PrintWindow($h, $hdc, 3) | Out-Null
$g.ReleaseHdc($hdc); $bmp.Save($outPath); $g.Dispose(); $bmp.Dispose()
Stop-Process -Id $p.Id -Force

Write-Output "Saved $outPath"
Get-Content $Log, "$Log.err" -ErrorAction SilentlyContinue |
	Select-String -Pattern "DICE|error|Error|fail|Fail|warn" | Select-Object -First 40 | ForEach-Object { $_.Line }
