param(
  [string]$LocalRoot = $env:R1_LOCAL_ROOT,
  [string]$PythonPath,
  [switch]$EnableVoice,
  [string]$VisionToolUrl,
  [switch]$TraceControl,
  [string]$LocalWakeModel,
  [string]$MimoVisionConfig,
  [switch]$WakeTraceText,
  [switch]$DirectSceneSpeech,
  [switch]$ProductMode,
  [ValidateRange(0,1800)][int]$DurationSeconds = 600,
  [ValidateRange(1,300)][int]$SessionSeconds = 60,
  [ValidateRange(0,3)][int]$DemoSessions = 0,
  [switch]$CheckOnly
)
$ErrorActionPreference = 'Stop'
if ($ProductMode -and (-not $EnableVoice -or -not $VisionToolUrl -or $DemoSessions -or
    $DurationSeconds -ne 0 -or $PSBoundParameters.ContainsKey('SessionSeconds'))) {
  throw 'ProductMode requires voice+vision, DurationSeconds0, without DemoSessions/SessionSeconds. Nothing started.'
}
if (-not $ProductMode -and $DurationSeconds -eq 0) { throw 'Untimed voice launcher requires ProductMode. Nothing started.' }
if (($DemoSessions -and (-not $EnableVoice -or -not $VisionToolUrl)) -or
    (-not $DemoSessions -and $SessionSeconds -gt 60)) {
  throw 'Longer/multiple rehearsal sessions require explicit DemoSessions and voice+vision URL. Nothing started.'
}
if ($DirectSceneSpeech -and (-not $EnableVoice -or -not $VisionToolUrl)) {
  throw 'DirectSceneSpeech requires an explicit voice+vision callback. Nothing started.'
}
if (-not $LocalRoot) {
  # Standard migration layout: <workspace>/development/<repo>/tools/agora.
  $LocalRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..\..\R1-local'))
}
if (-not $PythonPath) { $PythonPath = Join-Path $LocalRoot 'host-venv\Scripts\python.exe' }
if ($VisionToolUrl) {
  $toolUri = $null
  if (-not $EnableVoice -or -not [Uri]::TryCreate($VisionToolUrl,[UriKind]::Absolute,[ref]$toolUri) -or
      $toolUri.Scheme -ne 'https' -or $toolUri.AbsolutePath -ne '/vision/tool' -or
      $toolUri.UserInfo -or $toolUri.Query -or $toolUri.Fragment) {
    throw 'VisionToolUrl requires EnableVoice and an HTTPS /vision/tool URL. Nothing started.'
  }
}
$configPath = Join-Path $LocalRoot 'config\agora-lan.cfg'
$config = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json
$bindAddress = [string]$config.server_host
$port = [int]$config.server_port
if ($port -lt 1 -or $port -gt 65535 -or -not (Test-Path -LiteralPath $PythonPath -PathType Leaf)) {
  throw 'Invalid controller port or Python path. Nothing was started.'
}
# Read the selected address only; never connect/disconnect a network adapter.
$addresses = @([Net.NetworkInformation.NetworkInterface]::GetAllNetworkInterfaces() |
  ForEach-Object { $_.GetIPProperties().UnicastAddresses } |
  Where-Object { $_.Address.AddressFamily -eq [Net.Sockets.AddressFamily]::InterNetwork -and
                $_.Address.ToString() -eq $bindAddress })
if ($addresses.Count -eq 0) {
  throw "Saved controller IP $bindAddress is not present on this PC. Network was not changed."
}
$listeners = @([Net.NetworkInformation.IPGlobalProperties]::GetIPGlobalProperties().GetActiveTcpListeners())
if ($listeners | Where-Object { $_.Port -eq $port }) {
  throw "Port $port is already listening. Existing service was not stopped."
}
$serverArgs = @((Join-Path $PSScriptRoot 'session_server.py'), '--bind', $bindAddress,
  '--port', [string]$port, '--duration', [string]$DurationSeconds, '--vision')
if ($TraceControl) { $serverArgs += '--trace-control' }
if ($DirectSceneSpeech) { $serverArgs += '--direct-scene-speech' }
if ($DemoSessions) { $serverArgs += @('--demo-sessions',[string]$DemoSessions) }
if ($MimoVisionConfig) {
  if (-not [IO.Path]::IsPathRooted($MimoVisionConfig) -or
      -not (Test-Path -LiteralPath $MimoVisionConfig -PathType Leaf)) {
    throw 'MiMo vision requires an existing absolute local config path. Nothing started.'
  }
  $serverArgs += @('--mimo-vision-config', $MimoVisionConfig)
}
if ($WakeTraceText -and -not $LocalWakeModel) { throw 'WakeTraceText requires LocalWakeModel. Nothing started.' }
if ($LocalWakeModel) {
  if (-not (Test-Path -LiteralPath (Join-Path $LocalWakeModel 'verified-inputs.json') -PathType Leaf)) {
    throw 'Verified local wake model is missing. Nothing started.'
  }
  $serverArgs += @('--local-wake-model', $LocalWakeModel)
  if ($WakeTraceText) { $serverArgs += '--wake-trace-text' }
}
if ($EnableVoice) {
  $serverArgs += '--allow-paid-pipeline'
  if ($ProductMode) { $serverArgs += '--product-mode' }
  else { $serverArgs += @('--session-duration', [string]$SessionSeconds) }
  if ($VisionToolUrl) {
    if ($port -eq 8767 -or ($listeners | Where-Object { $_.Port -eq 8767 })) {
      throw 'Tool-only port 8767 is unavailable. Existing service was not stopped.'
    }
    $serverArgs += @('--vision-tool-url', $VisionToolUrl)
  }
} else {
  $serverArgs += '--disable-voice'
}
if ($CheckOnly) {
  Write-Host "PASS: controller=$bindAddress`:$port; vision=yes; voice=$($EnableVoice.IsPresent). No COM, cloud or model request."
  return
}
if ($ProductMode) { Write-Host 'R1 daily controller: no test quotas; ready until middle key or wake. No automatic RTC join.' }
else { Write-Host "R1 demo controller: $DurationSeconds seconds / at most 20 manual image requests. No automatic capture." }
Write-Host 'Product firmware: side keys adjust volume; middle key toggles voice. Legacy firmware may map left to scene. Ctrl-C closes the controller.'
if ($EnableVoice) {
  $startLimit = if ($DemoSessions) {$DemoSessions} else {1}
  if ($ProductMode) { Write-Warning 'Daily voice enabled; services may charge during conversations. Current RTC credentials24h; session closes60s before expiry. Device-disappearance cleanup remains enabled.' }
  else { Write-Warning "Voice enabled: at most $startLimit manually started session(s), $SessionSeconds seconds each. Saved Agora pipeline may charge TTS/service fees." }
  Write-Host 'Middle short press: start/stop voice. Starting this script alone does not start audio.'
  if ($VisionToolUrl) { Write-Host 'Experimental voice-camera tool enabled; HTTPS forwarding target is loopback port 8767 only. No tunnel is created here.' }
} else {
  Write-Host 'Vision-only mode: voice start is disabled. No audio will be started by this controller.'
}
Write-Host 'Keep this window open. Do not run another controller on the same port.'
$priorRoot = $env:R1_LOCAL_ROOT
$priorTokenReference = $env:AGORA_TOKEN_REFERENCE
try {
  $env:R1_LOCAL_ROOT = $LocalRoot
  $env:AGORA_TOKEN_REFERENCE = Join-Path $LocalRoot 'tools\agora-token-reference'
  & $PythonPath @serverArgs
  if ($LASTEXITCODE -ne 0) { throw "Controller exited with code $LASTEXITCODE; inspect the output above." }
} finally {
  $env:R1_LOCAL_ROOT = $priorRoot
  $env:AGORA_TOKEN_REFERENCE = $priorTokenReference
}
