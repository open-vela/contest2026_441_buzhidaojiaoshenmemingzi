param(
  [string]$PortName = 'COM12',
  [int]$BaudRate = 115200,
  [string]$LogDirectory = 'G:\CIE\R1\logs',
  [ValidateRange(20, 180)][int]$TimeoutSeconds = 90,
  [switch]$CheckOnly,
  [switch]$SelfTest
)
$ErrorActionPreference = 'Stop'

function Read-AgoraStatus([string]$Reply) {
  $pattern = 'Agora (?<phase>idle|config|init|joining|active|stopping|blocked) ' +
    'run=(?<run>[01]) joined=(?<joined>[01]) silent=(?<silent>[01]) ' +
    'up=(?<up>\d+) down=(?<down>\d+) tx=(?<tx>\d+) rx=(?<rx>\d+) ' +
    'error=(?<error>-?\d+) sdk=(?<sdk>-?\d+) buffer=(?<buffer>-?\d+) ' +
    'audio=(?<audio>-?\d+) cleanup=(?<cleanup>-?\d+) control=(?<control>-?\d+) retained=(?<retained>[01])'
  $found = [regex]::Matches($Reply, $pattern)
  if ($found.Count -eq 0) { throw 'Incomplete Agora status; no success assumed.' }
  $last = $found[$found.Count - 1]
  $result = @{ phase = $last.Groups['phase'].Value }
  foreach ($key in @('run','joined','silent','up','down','tx','rx','error','sdk','buffer','audio','cleanup','control','retained')) {
    $result[$key] = [long]$last.Groups[$key].Value
  }
  return $result
}
function Assert-Clean($State) {
  foreach ($key in @('error','sdk','buffer','audio','cleanup','control','retained')) {
    if ($State[$key] -ne 0) { throw "Agora failed: $key=$($State[$key]); inspect this run's log." }
  }
  if ($State.phase -eq 'blocked') { throw 'Agora retained resources; reset before further tests.' }
}
if ($SelfTest) {
  $sample = 'Agora idle run=0 joined=0 silent=1 up=0 down=0 tx=0 rx=0 error=0 sdk=0 buffer=0 audio=0 cleanup=0 control=0 retained=0'
  $state = Read-AgoraStatus $sample
  Assert-Clean $state
  if ($state.phase -ne 'idle') { throw 'Parser selftest failed.' }
  $rejected = $false
  try { $null = Read-AgoraStatus 'Agora probe queued=1 ret=0 silent=1' } catch { $rejected = $true }
  if (-not $rejected) { throw 'Queue acceptance incorrectly treated as completed status.' }
  foreach ($key in @('error','sdk','buffer','audio','cleanup','control','retained')) {
    $bad = Read-AgoraStatus ($sample.Replace("$key=0", "$key=1"))
    $rejected = $false
    try { Assert-Clean $bad } catch { $rejected = $true }
    if (-not $rejected) { throw "Missed failure field: $key" }
  }
  Write-Host 'PASS: status parser and failure gates; no serial port or network opened.'
  exit 0
}
if ($CheckOnly) {
  Write-Host 'Commands: apctl status; Agora version/status/init-test/probe; conversation stop on timeout.'
  Write-Host 'Two silent probes require joined=1, then clean idle. No Flash write or audio start.'
  Write-Host 'PASS: check only; COM was not opened.'
  exit 0
}
$null = New-Item -ItemType Directory -Force -Path $LogDirectory
$logPath = Join-Path $LogDirectory ('agora-silent-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.log')
$logWriter = [IO.StreamWriter]::new($logPath, $false, [Text.UTF8Encoding]::new($false))
$logWriter.AutoFlush = $true
$serial = [IO.Ports.SerialPort]::new($PortName, $BaudRate, 'None', 8, 'One')
$serial.Encoding = [Text.UTF8Encoding]::new($false)
$serial.ReadTimeout = 100
$serial.WriteTimeout = 1000
$serial.DtrEnable = $false
$serial.RtsEnable = $false
$inFlight = $false
function Exchange([string]$Command, [string]$Expected) {
  if ($Command -notmatch '^(apctl status|vision_badge agora (version|status|init-test|probe)|vision_badge conversation stop)$') {
    throw 'Command is outside the silent test allowlist.'
  }
  $logWriter.WriteLine("`r`n> " + $Command)
  # Preserve asynchronous AP diagnostics between queries. Do not use them
  # as the new command's reply, but never silently discard the first error.
  if ($serial.BytesToRead -gt 0) { $logWriter.Write($serial.ReadExisting()) }
  foreach ($character in ($Command + "`r").ToCharArray()) {
    $serial.Write([string]$character)
    Start-Sleep -Milliseconds 8
  }
  $reply = [Text.StringBuilder]::new()
  $deadline = [DateTime]::UtcNow.AddSeconds(15)
  while ([DateTime]::UtcNow -lt $deadline) {
    if ($serial.BytesToRead -gt 0) {
      $piece = $serial.ReadExisting()
      [void]$reply.Append($piece)
      $logWriter.Write($piece)
      if ($reply.ToString() -match $Expected) { return $reply.ToString() }
    } else { Start-Sleep -Milliseconds 40 }
  }
  throw "No expected reply to '$Command'; see $logPath."
}
function Status {
  $reply = Exchange 'vision_badge agora status' 'retained=[01]\s*\(ret=-?\d+\)'
  $state = Read-AgoraStatus $reply
  Assert-Clean $state
  return $state
}
try {
  $serial.Open()
  $serial.Write("`r")
  Start-Sleep -Milliseconds 300
  $reply = Exchange 'apctl status' 'SMP SysTick cpu0/cpu1=\d+/\d+ sleep enter/return=\d+/\d+'
  if ($reply -notmatch 'AP state=READY\(2\) error=0' -or
      $reply -notmatch 'RPTUN state=CONNECTED\(4\) error=0' -or
      $reply -notmatch 'AP SMP state=PASSED\(4\) error=0' -or
      $reply -notmatch 'AP supervisor state=HEALTHY\(2\) reason=NONE\(0\)') {
    throw 'AP, RPTUN, SMP or supervisor is not ready.'
  }
  $null = Exchange 'vision_badge agora version' 'Agora SDK=\S+.*\(ret=0\)'
  $state = Status
  if ($state.run -ne 0 -or $state.phase -ne 'idle') { throw 'A session is already running; nothing was interrupted.' }
  foreach ($action in @('init-test','probe','probe')) {
    $inFlight = $true
    $null = Exchange "vision_badge agora $action" "Agora $action queued=1 ret=0 silent=1"
    $joined = $false
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
      Start-Sleep -Milliseconds 250
      $state = Status
      if ($state.silent -ne 1 -or $state.up -ne 0 -or $state.down -ne 0) { throw 'Unexpected audio activity; stopping test.' }
      if ($state.joined -eq 1) { $joined = $true }
      if ($state.run -eq 0 -and $state.phase -eq 'idle') { break }
    } while ([DateTime]::UtcNow -lt $deadline)
    if ($state.run -ne 0 -or $state.phase -ne 'idle') { throw 'Silent operation did not finish before timeout.' }
    $inFlight = $false
    if ($action -eq 'probe' -and -not $joined) { throw 'No joined=1 observed; room entry is not verified.' }
    Write-Host "PASS: $action finished with clean idle."
  }
  $logWriter.WriteLine("`r`nPASS: silent init, join/leave and re-entry; no audio quality claim.")
  Write-Host "Silent verification passed. Log: $logPath"
} finally {
  if ($serial.IsOpen -and $inFlight) {
    try {
      $null = Exchange 'vision_badge conversation stop' 'Agora stop requested'
      $deadline = [DateTime]::UtcNow.AddSeconds(60)
      $released = $false
      do {
        Start-Sleep -Milliseconds 250
        $reply = Exchange 'vision_badge agora status' 'retained=[01]\s*\(ret=-?\d+\)'
        $state = Read-AgoraStatus $reply
        if ($state.run -eq 0) {
          $released = $state.phase -eq 'idle' -and $state.retained -eq 0 -and $state.cleanup -eq 0
          break
        }
      } while ([DateTime]::UtcNow -lt $deadline)
      if (-not $released) { throw 'Cleanup did not finish cleanly.' }
      $logWriter.WriteLine("`r`nCleanup confirmed: idle, retained=0, cleanup=0. Original failure remains a failure.")
    } catch {
      $logWriter.WriteLine("`r`nWARN: cleanup not confirmed; inspect status/reset before retry.")
      Write-Warning 'Cleanup was not confirmed; check status or reset before retry.'
    }
  }
  if ($serial.IsOpen) { $serial.Close() }
  $serial.Dispose()
  $logWriter.Dispose()
}
