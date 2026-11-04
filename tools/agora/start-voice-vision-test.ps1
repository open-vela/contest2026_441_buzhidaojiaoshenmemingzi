# SPDX-License-Identifier: Apache-2.0
param(
  [string]$LocalRoot = $env:R1_LOCAL_ROOT,
  [string]$CloudflaredPath,
  [string]$PythonPath,
  [string]$LocalWakeModel,
  [switch]$WakeTraceText,
  [switch]$DirectSceneSpeech,
  [switch]$ProductMode,
  [ValidateRange(0,3)][int]$DemoSessions=0,
  [ValidateRange(1,300)][int]$SessionSeconds=60,
  [ValidateRange(0,1800)][int]$DurationSeconds=600,
  [string]$MimoVisionConfig,
  [switch]$CheckOnly
)
$ErrorActionPreference = 'Stop'
if (-not $LocalRoot) {
  $LocalRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..\..\R1-local'))
}
if (-not $CloudflaredPath) {
  $CloudflaredPath = Join-Path $LocalRoot 'tools\cloudflared\2026.10.3\cloudflared.exe'
}
if (-not (Test-Path -LiteralPath $CloudflaredPath -PathType Leaf)) {
  throw 'Missing official cloudflared binary. See tools/agora/BASIC_LOOP.md; nothing started.'
}
foreach ($name in @('config.yml','config.yaml')) {
  if (Test-Path -LiteralPath (Join-Path $env:USERPROFILE ".cloudflared\$name")) {
    throw 'Existing Cloudflare configuration requires review; it was not changed.'
  }
}
$demo = Join-Path $PSScriptRoot 'start-demo.ps1'
$modeArgs = @{}
if ($ProductMode) {
  if ($DemoSessions -or $DurationSeconds -ne 0 -or $PSBoundParameters.ContainsKey('SessionSeconds')) {
    throw 'ProductMode requires DurationSeconds0 and no demo/session limits. Nothing started.'
  }
  $modeArgs.ProductMode = $true
} else { $modeArgs.SessionSeconds = $SessionSeconds }
# Reuse every local configuration/port/cost check before opening the tunnel.
& $demo @modeArgs -LocalRoot $LocalRoot -EnableVoice -DurationSeconds $DurationSeconds -DemoSessions $DemoSessions `
  -PythonPath $PythonPath -LocalWakeModel $LocalWakeModel -WakeTraceText:$WakeTraceText -DirectSceneSpeech:$DirectSceneSpeech -MimoVisionConfig $MimoVisionConfig `
  -VisionToolUrl 'https://preflight.invalid/vision/tool' -CheckOnly
if ($CheckOnly) {
  if ($ProductMode) { Write-Host 'PASS: daily mode, no test quotas, standby until button/wake; tool-only tunnel. Nothing started.'; return }
  $startLimit=if($DemoSessions){$DemoSessions}else{1}
  Write-Host "PASS: temporary tunnel -> 127.0.0.1:8767 only; at most $startLimit manual session(s)/$SessionSeconds seconds each; controller $DurationSeconds seconds. Nothing started."
  return
}
$directory = Join-Path $LocalRoot ('logs\voice-vision-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
$null = New-Item -ItemType Directory -Path $directory
$tunnelLog = Join-Path $directory 'tunnel.log'
$tunnel = $null
try {
  $tunnel = Start-Process -FilePath $CloudflaredPath -ArgumentList @(
    'tunnel','--no-autoupdate','--metrics','127.0.0.1:0','--loglevel','info',
    '--url','http://127.0.0.1:8767') -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput (Join-Path $directory 'tunnel.stdout') -RedirectStandardError $tunnelLog
  $timer = [Diagnostics.Stopwatch]::StartNew()
  $toolUrl = $null
  while ($timer.Elapsed.TotalSeconds -lt 45 -and -not $tunnel.HasExited) {
    # Start-Process owns a live writer; permit it while reading the log.
    $output = ''
    if (Test-Path -LiteralPath $tunnelLog) {
      $stream = [IO.File]::Open($tunnelLog,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
      $reader = [IO.StreamReader]::new($stream)
      try { $output = $reader.ReadToEnd() } finally { $reader.Dispose() }
    }
    $match = [regex]::Match($output, 'https://[a-z0-9-]+\.trycloudflare\.com')
    if ($match.Success -and $output.Contains('Registered tunnel connection')) {
      $toolUrl = $match.Value + '/vision/tool'
      break
    }
    Start-Sleep -Milliseconds 200
    $tunnel.Refresh()
  }
  if (-not $toolUrl) { throw "Temporary HTTPS tunnel did not become ready; inspect $tunnelLog. No voice session started." }
  Write-Host 'Temporary HTTPS callback ready; only the tool listener is forwarded. No audio started.'
  & $demo @modeArgs -LocalRoot $LocalRoot -EnableVoice -VisionToolUrl $toolUrl -DurationSeconds $DurationSeconds -DemoSessions $DemoSessions -TraceControl `
    -PythonPath $PythonPath -LocalWakeModel $LocalWakeModel -WakeTraceText:$WakeTraceText -DirectSceneSpeech:$DirectSceneSpeech -MimoVisionConfig $MimoVisionConfig
} finally {
  if ($null -ne $tunnel) {
    $tunnel.Refresh()
    if (-not $tunnel.HasExited) { Stop-Process -Id $tunnel.Id -ErrorAction Stop; $tunnel.WaitForExit() }
    $tunnel.Dispose()
  }
}
