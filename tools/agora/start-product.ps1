# SPDX-License-Identifier: Apache-2.0
param(
  [string]$LocalRoot = $env:R1_LOCAL_ROOT,
  [string]$PythonPath,
  [string]$CloudflaredPath,
  [string]$LocalWakeModel,
  [string]$MimoVisionConfig,
  [switch]$CheckOnly
)
if (-not $LocalRoot) { $LocalRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..\..\R1-local')) }
if (-not $PythonPath) { $PythonPath = Join-Path $LocalRoot 'asr-venv\Scripts\python.exe' }
if (-not $LocalWakeModel) { $LocalWakeModel = Join-Path $LocalRoot 'models\sherpa-zh-en-98590b7' }
# Reuse the reviewed tool-only gateway, configuration checks and cleanup.
# Opening this service never joins RTC; the board's middle key/wake owns start.
& (Join-Path $PSScriptRoot 'start-voice-vision-test.ps1') -ProductMode -DurationSeconds 0 `
  -LocalRoot $LocalRoot -PythonPath $PythonPath -CloudflaredPath $CloudflaredPath `
  -LocalWakeModel $LocalWakeModel -MimoVisionConfig $MimoVisionConfig -CheckOnly:$CheckOnly
