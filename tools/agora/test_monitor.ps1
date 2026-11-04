$ErrorActionPreference = 'Stop'
$tokens=$null;$errors=$null
$ast=[System.Management.Automation.Language.Parser]::ParseFile(
  (Join-Path $PSScriptRoot 'monitor-device.ps1'),[ref]$tokens,[ref]$errors)
if($errors.Count){throw 'Monitor parser errors.'}
$definition=$ast.Find({param($node)
  $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
  $node.Name -eq 'Get-R1MonitorReply'
},$true)
if($null -eq $definition){throw 'Reply parser missing.'}
Set-Item Function:\Get-R1MonitorReply -Value ($definition.Body.GetScriptBlock())
$sender=$ast.Find({param($node)
  $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
  $node.Name -eq 'Send-R1MonitorCommand'
},$true)
if($null -eq $sender){throw 'Paced sender missing.'}
Set-Item Function:\Send-R1MonitorCommand -Value ($sender.Body.GetScriptBlock())
function Assert-True([bool]$condition) { if(-not $condition){throw 'Monitor assertion failed.'} }
$fake=[pscustomobject]@{ Text=[Text.StringBuilder]::new(); Writes=0 }
$fake | Add-Member ScriptMethod Write {
  param([string]$value)
  if($value.Length -ne 1){throw 'Burst write is not allowed.'}
  [void]$this.Text.Append($value);$this.Writes++
}
Send-R1MonitorCommand $fake 'vision_badge agora netstats'
Assert-True ($fake.Text.ToString() -eq "vision_badge agora netstats`r")
Assert-True ($fake.Writes -eq $fake.Text.Length)
Assert-True ($null -eq (Get-R1MonitorReply 'nsh> Agora idle' 'conversation'))
Assert-True ($null -eq (Get-R1MonitorReply 'vision_badge: Agora idle run=0' 'conversation'))
$reply=Get-R1MonitorReply "noise`r`nvision_badge: Agora idle run=0 (AP status=0)`r`nnsh>" 'conversation'
Assert-True ($reply.Status -eq 0 -and -not $reply.RpcFailure)
Assert-True ($null -eq (Get-R1MonitorReply 'vision_badge: Agora stats captured=0 (ret=0)' 'conversation'))
foreach($case in @(@('stats','Agora stats captured=0'),@('rxstats','Agora RX callbacks=0'),
  @('netstats','Agora net enomem=0'),@('camera','camera: ready=1'))) {
  $reply=Get-R1MonitorReply "vision_badge: $($case[1]) (ret=0)" $case[0]
  Assert-True ($reply.Status -eq 0 -and -not $reply.RpcFailure)
}
$reply=Get-R1MonitorReply 'vision_badge: conversation RPC failed=-110' 'conversation'
Assert-True ($reply.Status -eq -110 -and $reply.RpcFailure)
$reply=Get-R1MonitorReply 'vision_badge: camera: ready=0 (ret=-16)' 'camera'
Assert-True ($reply.Status -eq -16 -and -not $reply.RpcFailure)
& (Join-Path $PSScriptRoot 'monitor-device.ps1') -CheckOnly
Write-Output 'PASS: real monitor parser and paced single-character sender; no COM or cloud request.'
