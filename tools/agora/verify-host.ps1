param(
  [string]$Python = 'G:\SoftWare\Anaconda\python.exe',
  [string]$ImageDirectory = 'G:\CIE\R1\firmware\agora-current',
  [string]$LogDirectory = 'G:\CIE\R1\logs',
  [switch]$LiveCloud
)
$ErrorActionPreference = 'Stop'
$pythonPath = (Get-Command $Python -ErrorAction Stop).Source
$null = New-Item -ItemType Directory -Force -Path $LogDirectory
$logPath = Join-Path $LogDirectory ('agora-host-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.log')
$logWriter = [IO.StreamWriter]::new($logPath, $false, [Text.UTF8Encoding]::new($false))
$logWriter.AutoFlush = $true
function Write-Log([string]$Text) {
  $logWriter.WriteLine($Text)
  Write-Host $Text
}
function Run-Check([string]$Label, [string]$Program, [string[]]$Arguments) {
  Write-Log ("`r`nCHECK: " + $Label)
  # Arguments never include account keys or board credentials.
  $errorPreference = $ErrorActionPreference
  try {
    $ErrorActionPreference = 'Continue'
    $lines = @(& $Program @Arguments 2>&1)
    $code = $LASTEXITCODE
  } finally { $ErrorActionPreference = $errorPreference }
  $lines | ForEach-Object { Write-Log $_.ToString() }
  if ($code -ne 0) {
    Write-Log "FAIL: $Label (exit=$code)"
    throw "$Label failed (exit=$code). Log: $logPath"
  }
}
Push-Location -LiteralPath $PSScriptRoot
try {
  Run-Check 'pinned token reference hashes' 'powershell.exe' @(
    '-NoProfile','-ExecutionPolicy','Bypass','-File',"$PSScriptRoot/bootstrap-token-reference.ps1",'-CheckOnly')
  Run-Check 'offline server lifecycle and HTTP checks' $pythonPath @(
    '-c','import sys, unittest; suite = unittest.defaultTestLoader.discover(''.'', pattern=''test_server.py''); result = unittest.TextTestRunner(stream=sys.stdout, verbosity=2).run(suite); sys.exit(not result.wasSuccessful())')
  Run-Check 'offline stream parser checks' $pythonPath @(
    '-c','import sys, unittest; suite = unittest.defaultTestLoader.discover(''.'', pattern=''test_stream.py''); result = unittest.TextTestRunner(stream=sys.stdout, verbosity=2).run(suite); sys.exit(not result.wasSuccessful())')
  Run-Check 'offline MiMo TTS bridge and free-only gates' $pythonPath @(
    '-c','import sys, unittest; suite = unittest.defaultTestLoader.discover(''.'', pattern=''test_mimo_tts_bridge.py''); result = unittest.TextTestRunner(stream=sys.stdout, verbosity=2).run(suite); sys.exit(not result.wasSuccessful())')
  Run-Check 'offline scene JPEG/HTTP and explicit paid-route checks' $pythonPath @(
    '-c','import sys, unittest; suite = unittest.defaultTestLoader.discover(''.'', pattern=''test_scene_relay.py''); result = unittest.TextTestRunner(stream=sys.stdout, verbosity=2).run(suite); sys.exit(not result.wasSuccessful())')
  Run-Check 'serial verifier selftest, no COM opened' 'powershell.exe' @(
    '-NoProfile','-ExecutionPolicy','Bypass','-File',"$PSScriptRoot/verify-silent.ps1",'-SelfTest')
  Run-Check 'candidate hashes and partition ranges, no Flash write' 'powershell.exe' @(
    '-NoProfile','-ExecutionPolicy','Bypass','-File',"$PSScriptRoot/flash-candidate.ps1",'-CheckOnly','-ImageDirectory',$ImageDirectory)
  if ($LiveCloud) {
    Run-Check 'live provider streaming and test context, no audio' $pythonPath @(
      "$PSScriptRoot/session_control.py",'provider-stream-test')
    Run-Check 'live cloud join/query/leave, no board audio' $pythonPath @(
      "$PSScriptRoot/session_control.py",'probe')
  }
  Write-Log "`r`nPASS: host verification; hardware results are still pending."
  Write-Host "PASS. No microphone, speaker, serial or Flash access. Log: $logPath"
} finally { Pop-Location; $logWriter.Dispose() }
