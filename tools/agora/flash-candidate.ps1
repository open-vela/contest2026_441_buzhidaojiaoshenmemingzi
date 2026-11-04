param(
    [switch]$CheckOnly,
    [Parameter(Mandatory=$true)][string]$ImageDirectory,
    [string]$ToolDirectory,
    [string]$PortName,
    [string]$LogDirectory,
    [ValidateRange(1,180)][int]$GetBusTimeoutSeconds = 15,
    [ValidateSet(115200,230400,460800,921600)][int]$BaudRate = 115200
)
$ErrorActionPreference = 'Stop'
$imageDir = (Resolve-Path -LiteralPath $ImageDirectory).Path
if ($PortName -and ($PortName -notmatch '^COM([1-9][0-9]{0,3})$' -or
    [int]$Matches[1] -gt 4096)) { throw 'Specify an actual COM1..COM4096 port.' }
if ($imageDir.IndexOfAny([char[]]',@') -ge 0) {
    throw 'The loader multi-image syntax does not support commas or @ in the image path.'
}
$manifest = Get-Content -LiteralPath (Join-Path $imageDir 'build-manifest.json') -Raw | ConvertFrom-Json
$layoutHash = '34dfff5891ff2b05416d602cedd7188c796b42ccd41a433911526cbf1ed849f2'
if ($manifest.layout.sha256 -ne $layoutHash -or $manifest.boot -ne 'direct') {
    throw 'Wrong partition layout or boot mode. Nothing was written.'
}
$ranges = @(
    @{Name='boot'; Offset=0x000000; Capacity=0x11000},
    @{Name='cp'; Offset=0x011000; Capacity=0x132000},
    @{Name='ap'; Offset=0x143000; Capacity=0x187000},
    @{Name='pair'; Offset=0x2CA000; Capacity=0x2B9000}
)
$items = @()
foreach ($range in $ranges) {
    $path = Join-Path $imageDir ($range.Name + '.bin')
    $image = Get-Item -LiteralPath $path
    $expected = $manifest.finalized_flash.($range.Name)
    $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($null -eq $expected -or $image.Length -ne $expected.size -or
        $hash -ne $expected.sha256 -or $image.Length -le 0 -or
        $image.Length -gt $range.Capacity) {
        throw ('Image or range check failed: ' + $range.Name)
    }
    Write-Host ('{0}: address=0x{1:X6} bytes={2} SHA256={3}' -f
        $range.Name,$range.Offset,$image.Length,$hash)
    $items += ('{0}@0x{1:x}-0x{2:x}' -f $path.Replace('\','/'),$range.Offset,$image.Length)
}
if ($CheckOnly) { Write-Host 'PASS. Check only; no serial port was opened.'; return }
if (-not $PortName) { throw 'PortName is required for writing; no default COM port is selected.' }
if (-not $ToolDirectory) { throw 'ToolDirectory is required for writing.' }
$portNumber = [int]$PortName.Substring(3)
$toolDir = (Resolve-Path -LiteralPath $ToolDirectory).Path
if (-not (Test-Path -LiteralPath (Join-Path $toolDir 'bk_loader.exe'))) {
    throw 'bk_loader.exe is missing.'
}
if (-not $LogDirectory) { $LogDirectory = Join-Path $imageDir 'flash-logs' }
$null = New-Item -ItemType Directory -Path $LogDirectory -Force
$logPath = Join-Path $LogDirectory ((Split-Path -Leaf $imageDir) + '-' +
    (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '-flash.log')
Write-Host 'Writes boot/CP/AP/pair only. RF, EasyFlash and user data are preserved.'
Write-Host ('Close BKFIL and every serial terminal using ' + $PortName + '.')
Write-Host 'Image checks passed. Starting Flash write automatically.'
Push-Location -LiteralPath $toolDir
try {
    & '.\bk_loader.exe' download -p $portNumber -b $BaudRate --uart-type CH340 --getbus-timeout $GetBusTimeoutSeconds `
        --mainBin-multi ($items -join ',') --reboot 1 --fast-link 1 2>&1 |
        Tee-Object -FilePath $logPath
    $loaderExit = $LASTEXITCODE
} finally {
    Pop-Location
}
$hasFlashOk = [bool](Select-String -LiteralPath $logPath -SimpleMatch 'Writing Flash OK' -Quiet)
Write-Host ('Loader exit={0}; Writing Flash OK marker={1}' -f $loaderExit,$hasFlashOk)
if ($loaderExit -ne 0 -or -not $hasFlashOk) {
    throw ('Loader result rejected (exit={0}, OK marker={1}). Read {2}' -f $loaderExit,$hasFlashOk,$logPath)
}
Write-Host ('Writing Flash OK confirmed. Open ' + $PortName + ' at 115200 for verification.')
