<#
.SYNOPSIS
  Decode-only performance test for the AviSynth+ MxfSource adapter.

.DESCRIPTION
  Drives the plugin through ffmpeg's real "-f avisynth" demuxer (the actual
  intended production pipeline) with "-f null -" as the output, so frames
  are fully decoded and copied out of the plugin but never re-encoded --
  this isolates plugin decode throughput from any encoder cost.

  If -Mxf is omitted, generates a synthetic uncompressed-UYVY test file via
  gen_test_mxf.exe (build target: `cmake --build build --target gen_test_mxf`).
  Uncompressed essence exercises the ring-buffer/decode-thread/copy path but
  not real codec decode; pass -Mxf pointing at a real MPEG-2/DNxHD/ProRes/etc.
  file to also measure actual codec decode cost.

.PARAMETER Mxf
  Path to an existing .mxf file to benchmark. If omitted, one is generated.

.PARAMETER Frames
  Frame count for the generated synthetic file (ignored if -Mxf is given).

.PARAMETER Width / -Height
  Resolution for the generated synthetic file (ignored if -Mxf is given).

.PARAMETER BufferFrames
  MxfSource's buffer_frames argument (ring buffer depth).

.PARAMETER PluginDll
  Path to mxfsource.dll. Defaults to build/src/avs/Release/mxfsource.dll
  relative to the repo root.

.EXAMPLE
  .\tools\benchmark_avs.ps1
  .\tools\benchmark_avs.ps1 -Frames 300 -Width 1920 -Height 1080
  .\tools\benchmark_avs.ps1 -Mxf E:\20201112_001299.mxf
#>
param(
  [string]$Mxf,
  [int]$Frames = 300,
  [int]$Width = 1280,
  [int]$Height = 720,
  [int]$BufferFrames = 300,
  [string]$PluginDll,
  [string]$WorkDir
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot

if (-not $PluginDll) {
  $PluginDll = Join-Path $repoRoot "build\src\avs\Release\mxfsource.dll"
}
if (-not (Test-Path $PluginDll)) {
  throw "Plugin DLL not found at $PluginDll -- build it first: cmake --build build --config Release --target mxfsource_avs"
}
if (-not $WorkDir) {
  $WorkDir = Join-Path ([System.IO.Path]::GetTempPath()) "mxfsrc_bench"
}
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null

if (-not $Mxf) {
  $genTool = Join-Path $repoRoot "build\tests\mxf_parser_tests\Release\gen_test_mxf.exe"
  if (-not (Test-Path $genTool)) {
    throw "gen_test_mxf.exe not found at $genTool -- build it first: cmake --build build --config Release --target gen_test_mxf"
  }
  $Mxf = Join-Path $WorkDir "bench_${Frames}f_${Width}x${Height}.mxf"
  Write-Host "Generating synthetic test file: $Frames frames, ${Width}x${Height} uncompressed UYVY..."
  & $genTool $Mxf $Frames $Width $Height
}

$mxfSize = (Get-Item $Mxf).Length
$avsPath = Join-Path $WorkDir "bench.avs"
$dllEscaped = $PluginDll.Replace('\', '\\')
$mxfEscaped = $Mxf.Replace('\', '\\')
Set-Content -Path $avsPath -Value @"
LoadPlugin("$dllEscaped")
MxfSource("$mxfEscaped", buffer_frames=$BufferFrames)
"@

$outNull = Join-Path $WorkDir "bench_out.null"
$errFile = Join-Path $WorkDir "bench_err.txt"

Write-Host "Running: ffmpeg -f avisynth -i $avsPath -f null -"
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$proc = Start-Process -FilePath "ffmpeg" `
  -ArgumentList "-y", "-hide_banner", "-f", "avisynth", "-i", "`"$avsPath`"", "-f", "null", "-" `
  -RedirectStandardError $errFile -Wait -NoNewWindow -PassThru
$sw.Stop()

if ($proc.ExitCode -ne 0) {
  Write-Host "--- ffmpeg stderr ---"
  Get-Content $errFile | Select-Object -Last 30
  throw "ffmpeg exited with code $($proc.ExitCode)"
}

$stderrLines = Get-Content $errFile
$summaryLine = $stderrLines | Where-Object { $_ -match "^frame=\s*\d+" } | Select-Object -Last 1
$streamLine = $stderrLines | Where-Object { $_ -match "Video:.*\d+x\d+" } | Select-Object -Last 1

$frameCount = $null
$ffmpegSpeed = $null
if ($summaryLine -match "frame=\s*(\d+)") { $frameCount = [int]$Matches[1] }
if ($summaryLine -match "speed=\s*([\d.]+)x") { $ffmpegSpeed = [double]$Matches[1] }

$wallSeconds = $sw.Elapsed.TotalSeconds
$decodeFps = if ($frameCount -and $wallSeconds -gt 0) { $frameCount / $wallSeconds } else { $null }
$throughputMBs = if ($wallSeconds -gt 0) { ($mxfSize / 1MB) / $wallSeconds } else { $null }

Write-Host ""
Write-Host "=== MxfSource decode-only benchmark ==="
Write-Host ("File:            {0}" -f $Mxf)
Write-Host ("File size:       {0:N1} MB" -f ($mxfSize / 1MB))
if ($streamLine) { Write-Host ("Stream:          {0}" -f $streamLine.Trim()) }
Write-Host ("Buffer frames:   {0}" -f $BufferFrames)
Write-Host ("Frames decoded:  {0}" -f $frameCount)
Write-Host ("Wall time:       {0:N3} s" -f $wallSeconds)
Write-Host ("Decode fps:      {0:N1} fps" -f $decodeFps)
Write-Host ("Throughput:      {0:N1} MB/s" -f $throughputMBs)
if ($ffmpegSpeed) { Write-Host ("ffmpeg speed:    {0:N1}x realtime (per source fps)" -f $ffmpegSpeed) }
