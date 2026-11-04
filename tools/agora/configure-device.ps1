param(
  [string]$ConfigPath = 'G:\CIE\R1\config\agora-lan.cfg',
  [string]$PortName = 'COM12',
  [int]$BaudRate = 115200,
  [ValidateRange(-1, 100)][int]$Volume = -1,
  [switch]$CheckOnly
)
$ErrorActionPreference = 'Stop'
$config = Get-Content -LiteralPath $ConfigPath -Raw | ConvertFrom-Json
if ($Volume -ge 0) { $config.volume = $Volume }
if ($config.app_id -notmatch '^[a-fA-F0-9]{32}$' -or
    $config.device_key -notmatch '^[a-f0-9]{64}$' -or
    $config.server_host -notmatch '^\d{1,3}(\.\d{1,3}){3}$' -or
    $config.server_port -lt 1 -or $config.server_port -gt 65535 -or
    $config.volume -lt 0 -or $config.volume -gt 100) {
  throw 'Invalid LAN controller configuration. Nothing was sent.'
}
$safe = [ordered]@{
  app_id = [string]$config.app_id
  server_host = [string]$config.server_host
  server_port = [int]$config.server_port
  device_key = [string]$config.device_key
  volume = [int]$config.volume
}
$json = $safe | ConvertTo-Json -Compress
$command = "vision_badge agora-config '$json'"
if ($command.Length -ge 256) { throw 'Configuration command exceeds the NSH line limit.' }
if ($CheckOnly) {
  Write-Host "PASS. Configuration length=$($command.Length); COM was not opened."
  exit 0
}
$serial = [IO.Ports.SerialPort]::new($PortName, $BaudRate, 'None', 8, 'One')
$serial.Encoding = [Text.UTF8Encoding]::new($false)
$serial.ReadTimeout = 100
$serial.WriteTimeout = 1000
$serial.DtrEnable = $false
$serial.RtsEnable = $false
function Wait-Prompt([string]$Expected = 'nsh>') {
  $reply = [Text.StringBuilder]::new()
  $deadline = [DateTime]::UtcNow.AddSeconds(15)
  while ([DateTime]::UtcNow -lt $deadline) {
    if ($serial.BytesToRead -gt 0) {
      [void]$reply.Append($serial.ReadExisting())
      if ($reply.ToString() -match $Expected) { return $reply.ToString() }
    } else { Start-Sleep -Milliseconds 40 }
  }
  throw 'NSH did not return. Check board state and serial port; secret output suppressed.'
}
try {
  $serial.Open()
  $serial.DiscardInBuffer()
  $serial.Write("`r")
  $null = Wait-Prompt
  $serial.DiscardInBuffer()
  foreach ($character in ($command + "`r").ToCharArray()) {
    $serial.Write([string]$character)
    Start-Sleep -Milliseconds 8
  }
  # A prompt can be redrawn by unrelated logs before the RPC reply arrives.
  $reply = Wait-Prompt '\(ret=-?\d+\)'
  if ($reply -notmatch '\(ret=0\)') {
    $status = if ($reply -match '\(ret=(-?\d+)\)') { $Matches[1] } else { 'unknown' }
    throw "AP did not confirm the configuration (status=$status). No microphone or speaker was started."
  }
} finally {
  if ($serial.IsOpen) { $serial.Close() }
  $serial.Dispose()
  $command = $json = $reply = $null
}
Write-Host "AP confirmed Agora LAN configuration saved, volume=$($safe.volume)%. No recording, playback or Flash write was performed."
