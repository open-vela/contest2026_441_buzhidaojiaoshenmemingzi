param(
  [string]$PortName = 'COM12',
  [ValidateRange(1, 600)][int]$DurationSeconds = 120,
  [ValidateRange(1, 60)][int]$IntervalSeconds = 5,
  [ValidateRange(6, 30)][int]$ReplyTimeoutSeconds = 12,
  [string]$LogPath,
  [switch]$CheckOnly
)
$ErrorActionPreference = 'Stop'

function Get-R1MonitorReply {
  param([string]$Text, [string]$Kind)
  $prefix = switch ($Kind) {
    'conversation' { 'Agora (?:idle|config|init|joining|active|stopping|blocked) ' }
    'stats' { 'Agora stats ' }
    'rxstats' { 'Agora RX ' }
    'netstats' { 'Agora net ' }
    'camera' { 'camera: ' }
    default { throw 'Unknown read-only query kind.' }
  }
  $reply = [regex]::Match($Text,
    "vision_badge: ${prefix}[^\r\n]*?\((?:ret|AP status)=(?<status>-?\d+)\)")
  if ($reply.Success) {
    return [pscustomobject]@{ Status=[int]$reply.Groups['status'].Value; RpcFailure=$false }
  }
  $failure = [regex]::Match($Text,
    'vision_badge: (?:conversation|Agora|camera) RPC failed=(?<status>-?\d+)')
  if ($failure.Success) {
    return [pscustomobject]@{ Status=[int]$failure.Groups['status'].Value; RpcFailure=$true }
  }
  return $null
}

function Send-R1MonitorCommand {
  param($Serial, [string]$Command)
  # Match the provisioning sender after an incomplete/delayed burst echo was
  # observed with Wi-Fi/camera logging. Never resend a pending command.
  foreach ($character in ($Command + "`r").ToCharArray()) {
    $Serial.Write([string]$character)
    Start-Sleep -Milliseconds 8
  }
}

# Read-only allowlist: never start/stop audio, save credentials or upload images.
$queries = @(
  @{Kind='conversation'; Command='vision_badge conversation status'},
  @{Kind='stats'; Command='vision_badge agora stats'},
  @{Kind='rxstats'; Command='vision_badge agora rxstats'},
  @{Kind='netstats'; Command='vision_badge agora netstats'},
  @{Kind='camera'; Command='vision_badge camera status'}
)
if ($CheckOnly) {
  Write-Output 'PASS: one outstanding read-only request; stops on RPC error/timeout; no COM opened.'
  return
}
$serial = [IO.Ports.SerialPort]::new($PortName,115200,'None',8,'One')
$serial.Encoding = [Text.UTF8Encoding]::new($false)
$serial.DtrEnable = $false
$serial.RtsEnable = $false
$serial.ReadTimeout = 100
$serial.WriteTimeout = 1000
$log = [Text.StringBuilder]::new()
$response = [Text.StringBuilder]::new()
$timer = [Diagnostics.Stopwatch]::new()
$pending = $false
$next = 0.0
$sentAt = 0.0
$index = 0
try {
  $serial.Open()
  $timer.Start()
  Write-Output 'READ_ONLY_MONITOR_READY: no automatic recording, playback, cloud join or model request.'
  while ($timer.Elapsed.TotalSeconds -lt $DurationSeconds -or $pending) {
    if ($serial.BytesToRead -gt 0) {
      $chunk = $serial.ReadExisting()
      [void]$log.Append($chunk)
      [Console]::Write($chunk)
      if ($pending) {
        [void]$response.Append($chunk)
        $reply = Get-R1MonitorReply $response.ToString() $query.Kind
        if ($null -ne $reply) {
          $elapsed = [int](1000 * ($timer.Elapsed.TotalSeconds - $sentAt))
          $timing = "MONITOR reply=$($query.Kind) elapsed_ms=$elapsed status=$($reply.Status)`r`n"
          [void]$log.Append($timing);[Console]::Write($timing)
          if ($reply.RpcFailure -or $reply.Status -ne 0) {
            throw 'Query failed; monitoring stopped instead of queueing more commands.'
          }
          $pending = $false
          $next = $timer.Elapsed.TotalSeconds + $IntervalSeconds
        }
      }
    }
    if ($pending -and $timer.Elapsed.TotalSeconds - $sentAt -ge $ReplyTimeoutSeconds) {
      throw 'No matching reply within deadline; no further commands were sent.'
    }
    if (-not $pending -and $timer.Elapsed.TotalSeconds -ge $next -and
        $timer.Elapsed.TotalSeconds -lt $DurationSeconds) {
      $query = $queries[$index % $queries.Count]
      $index++
      [void]$response.Clear()
      $sentAt = $timer.Elapsed.TotalSeconds
      Send-R1MonitorCommand $serial $query.Command
      $pending = $true
    }
    Start-Sleep -Milliseconds 40
  }
} finally {
  if ($serial.IsOpen) { $serial.Close() }
  $serial.Dispose()
  if ($LogPath) {
    $directory = Split-Path -Parent $LogPath
    if ($directory) { [void][IO.Directory]::CreateDirectory($directory) }
    [IO.File]::WriteAllText($LogPath,$log.ToString(),[Text.UTF8Encoding]::new($false))
  }
}
