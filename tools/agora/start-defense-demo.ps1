# SPDX-License-Identifier: Apache-2.0
param(
  [string]$LocalRoot,
  [string]$PythonPath,
  [string]$LocalWakeModel,
  [ValidateRange(1,3)][int]$MaxSessions=3,
  [ValidateRange(1,300)][int]$SessionSeconds=180,
  [ValidateRange(1,1800)][int]$ControllerSeconds=1200,
  [switch]$CheckOnly
)
$ErrorActionPreference='Stop'
if(-not $LocalRoot){$LocalRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..\..\R1-local'))}
if(-not $PythonPath){$PythonPath=Join-Path $LocalRoot 'asr-venv\Scripts\python.exe'}
if(-not $LocalWakeModel){$LocalWakeModel=Join-Path $LocalRoot 'models\sherpa-zh-en-98590b7'}
Write-Host "REHEARSAL PROFILE: up to $MaxSessions manual sessions, $SessionSeconds seconds each; maximum reserved voice seconds=$($MaxSessions*$SessionSeconds)."
Write-Host 'Starting the host does not join RTC. Wake/key-triggered voice and image use may charge. No auto-rejoin or model fallback.'
Write-Host 'Use only after product hardware acceptance. Direct scene speech optimization and text tracing remain off.'
& (Join-Path $PSScriptRoot 'start-voice-vision-test.ps1') -LocalRoot $LocalRoot -PythonPath $PythonPath `
  -LocalWakeModel $LocalWakeModel -DemoSessions $MaxSessions -SessionSeconds $SessionSeconds `
  -DurationSeconds $ControllerSeconds -CheckOnly:$CheckOnly
