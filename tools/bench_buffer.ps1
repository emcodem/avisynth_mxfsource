# Buffer-size benchmark through the AviSynth plugin (ffmpeg -f avisynth, -f null).
# For each buffer_frames/eagerdecode combo (0,0 = auto) and each chain, starting
# after a jump (Trim at -Start): fps, peak memory, and the plugin's serve stats
# (buffered / fallback / restarts / auto growth). See TODO.md, "Default
# buffer_frames / eagerdecode", for the 2026-11-07 results.
# Example: .\tools\bench_buffer.ps1 -Mxf D:/test/uhd.mxf -Frames 600 -Combos @(,@(0,0))
param(
  [Parameter(Mandatory)] [string]$Mxf,
  [int]$Start = 1250,
  [int]$Frames = 3000,
  [int[][]]$Combos = @(@(0, 0), @(300, 32), @(150, 32), @(60, 8), @(30, 8), @(16, 4)),
  [string]$Out
)
$dll = (Join-Path (Split-Path -Parent $PSScriptRoot) "build/src/avs/Release/mxfsource.dll").Replace('\', '/')
$work = Join-Path ([System.IO.Path]::GetTempPath()) "mxfsrc_bench_buffer"
New-Item -ItemType Directory -Force $work | Out-Null
$end = $Start + $Frames - 1
$chains = [ordered]@{
  "seq"         = { param($c) "$c.Trim($Start,$end)" }
  "pf2_r1"      = { param($c) "$c.Trim($Start,$end).TemporalSoften(1,4,8,mode=2).Prefetch(2)" }
  "pf8_r1"      = { param($c) "$c.Trim($Start,$end).TemporalSoften(1,4,8,mode=2).Prefetch(8)" }
  "pf8_r3"      = { param($c) "$c.Trim($Start,$end).TemporalSoften(3,4,8,mode=2).Prefetch(8)" }
  "pf16_r3"     = { param($c) "$c.Trim($Start,$end).TemporalSoften(3,4,8,mode=2).Prefetch(16)" }
  "pf32_r3"     = { param($c) "$c.Trim($Start,$end).TemporalSoften(3,4,8,mode=2).Prefetch(32)" }
  "back300"     = { param($c) "$c.Trim($Start,$($Start+300)).Reverse()" }
}
$rows = @()
foreach ($combo in $Combos) {
  $buf = $combo[0]; $eager = $combo[1]
  foreach ($name in $chains.Keys) {
    $bargs = if ($buf -gt 0) { ", buffer_frames=$buf, eagerdecode=$eager" } else { "" }
    $src = "MXFSource(`"$Mxf`"$bargs, debug=true).KillAudio()"
    $body = & $chains[$name] $src
    $n = if ($name -eq "back300") { 301 } else { $Frames }
    $avs = Join-Path $work "b_${buf}_${eager}_$name.avs"
    "LoadPlugin(`"$dll`")`r`n$body" | Set-Content -Encoding ascii $avs
    $p = New-Object System.Diagnostics.Process
    $p.StartInfo.FileName = "ffmpeg"
    $p.StartInfo.Arguments = "-v error -i `"$avs`" -f null -"
    $p.StartInfo.UseShellExecute = $false
    $p.StartInfo.RedirectStandardError = $true
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $null = $p.Start()
    $errTask = $p.StandardError.ReadToEndAsync()
    $peak = 0
    while (-not $p.HasExited) {
      try { $p.Refresh(); if ($p.PeakWorkingSet64 -gt $peak) { $peak = $p.PeakWorkingSet64 } } catch {}
      Start-Sleep -Milliseconds 100
    }
    $sw.Stop()
    $stderr = $errTask.Result
    $stat = ""
    if ($stderr -match "served (\d+) frame\(s\) from buffer, (\d+) frame\(s\) via fallback[^,:]*(, buffer grown (\d+) time\(s\) to (\d+) frames)?(, decoder restarted (\d+))?") {
      $restarts = if ($Matches[7]) { $Matches[7] } else { 0 }
      $grow = if ($Matches[4]) { "$($Matches[4])->$($Matches[5])" } else { 0 }
      $stat = "buf=$($Matches[1]) fb=$($Matches[2]) rs=$restarts grow=$grow"
    }
    $errLines = ($stderr -split "`n" | Where-Object { $_ -match "error|Error" -and $_ -notmatch "served" } | Select-Object -First 1)
    $fps = [math]::Round($n / $sw.Elapsed.TotalSeconds, 1)
    $msStep = [math]::Round($sw.Elapsed.TotalMilliseconds / $n, 1)
    $row = "{0,4}/{1,-3} {2,-8} {3,7} fps {4,6} ms/f peak {5,6} MB  {6} {7}" -f $buf, $eager, $name, $fps, $msStep, [math]::Round($peak / 1MB), $stat, $errLines
    $row
    $rows += $row
  }
}
if ($Out) { $rows | Set-Content $Out }
