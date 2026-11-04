param([string]$Destination = 'G:\CIE\R1\tools\agora-token-reference', [switch]$CheckOnly)
$ErrorActionPreference = 'Stop'
$commit = '2e736ebefdb7ffb2832ae1a6f95039d8589e1f85'
$base = "https://raw.githubusercontent.com/AgoraIO-Community/Conversational-AI-IOT-Sample/$commit"
$files = @(
  @{Name='RtcTokenBuilder2.py'; Path='server/aiot_server_demo_example/RtcTokenBuilder2.py'; Hash='2520fe456b33f814ed3d79d9d566b21f73fd5f66cc2d2fe972c396bbf3143246'},
  @{Name='AccessToken2.py'; Path='server/aiot_server_demo_example/AccessToken2.py'; Hash='86e882a9cdefbae6d756ef138546126f1dc0a58787dc7aab15d66a5218ef5418'},
  @{Name='Packer.py'; Path='server/aiot_server_demo_example/Packer.py'; Hash='e7075735193b5558a454b7e41541da77c3d331b61f9ba0735f22864bd88ff357'},
  @{Name='LICENSE'; Path='LICENSE'; Hash='fcb4483f56e31f94054af527ce3e8a84aa2491b661db25207340887c17fa8750'}
)
if (-not $CheckOnly) { $null = New-Item -ItemType Directory -Path $Destination -Force }
foreach ($item in $files) {
  $target = Join-Path $Destination $item.Name
  if (Test-Path -LiteralPath $target) {
    if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant() -ne $item.Hash) {
      throw ('Existing reference does not match pinned source: ' + $item.Name)
    }
  } else {
    if ($CheckOnly) { throw ('Pinned reference missing: ' + $item.Name + '; run bootstrap without -CheckOnly to install.') }
    $temporary = Join-Path $Destination ($item.Name + '.download')
    Invoke-WebRequest -UseBasicParsing -Uri ($base + '/' + $item.Path) -OutFile $temporary
    if ((Get-FileHash -LiteralPath $temporary -Algorithm SHA256).Hash.ToLowerInvariant() -ne $item.Hash) {
      throw ('Downloaded reference does not match pinned source: ' + $item.Name)
    }
    Move-Item -LiteralPath $temporary -Destination $target
  }
  Write-Host ('Verified pinned Agora reference: ' + $item.Name)
}
