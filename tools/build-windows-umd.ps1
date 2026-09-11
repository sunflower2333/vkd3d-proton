param([ValidateSet('arm64','x64','x86')][string]$Architecture = 'arm64')
$ErrorActionPreference = 'Stop'
$sdkVersion = $env:WindowsSDKVersion.TrimEnd('\')
if (!$sdkVersion) { throw 'MSVC environment has no selected SDK version' }
$kit = Join-Path $env:WindowsSdkDir ('Include\' + $sdkVersion)
if (!(Test-Path (Join-Path $kit 'um\d3d12umddi.h'))) {
    $installer = Join-Path $env:RUNNER_TEMP 'vkd3d-wdksetup.exe'
    Invoke-WebRequest 'https://go.microsoft.com/fwlink/?linkid=2272234' -OutFile $installer
    $process = Start-Process $installer -ArgumentList '/quiet','/norestart','/features','+' -PassThru
    $null = $process.Handle
    $process.WaitForExit()
    if ($process.ExitCode -notin @(0,3010)) { throw "WDK installer failed: $($process.ExitCode)" }
}
foreach ($header in @('windows.h','d3d12umddi.h','d3d10umddi.h','d3dumddi.h','d3dkmddi.h','d3dukmdt.h')) {
    $found = Get-ChildItem -Path ($env:INCLUDE -split ';' | Where-Object { $_ -and (Test-Path $_) }) `
        -Filter $header -File -ErrorAction SilentlyContinue | Select-Object -First 1
    if (!$found -or $found.FullName -notmatch [regex]::Escape($sdkVersion)) {
        throw "Header $header is missing or does not resolve in SDK/WDK $sdkVersion"
    }
    Write-Output "$header=$($found.FullName)"
}
$buildDir = 'build-umd-' + $Architecture
$crossArgs = @()
if ($Architecture -eq 'arm64') { $crossArgs = @('--cross-file', 'tools/umd-arm64-msvc.ini') }
meson setup $buildDir @crossArgs --buildtype release -Ddebug=true -Denable_umd_bridge=true -Denable_umd_bridge_tests=true
if ($LASTEXITCODE) { exit $LASTEXITCODE }
meson compile -C $buildDir -j 3 viogpud3d12 vkd3d-umd-ddi-abi-test vkd3d-umd-ddi-descriptor-test
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$dll = Join-Path $buildDir 'libs\vkd3d-umd\viogpud3d12.dll'
$test = Join-Path $buildDir 'libs\vkd3d-umd\vkd3d-umd-ddi-abi-test.exe'
if ($Architecture -ne 'arm64') {
    & $test (Resolve-Path $dll).Path
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    & (Join-Path $buildDir 'libs\vkd3d-umd\vkd3d-umd-ddi-descriptor-test.exe')
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
}
$output = Join-Path $buildDir 'package'
New-Item -ItemType Directory -Force $output | Out-Null
Copy-Item $dll,$test,(Join-Path $buildDir 'libs\vkd3d-umd\viogpud3d12.pdb') $output
Copy-Item (Join-Path $buildDir 'libs\vkd3d-umd\vkd3d-umd-ddi-descriptor-test.exe') $output
Copy-Item libs/vkd3d-umd/README.md $output
dumpbin /headers $dll | Out-File (Join-Path $output 'pe-headers.txt')
dumpbin /exports $dll | Out-File (Join-Path $output 'exports.txt')
dumpbin /dependents $dll | Out-File (Join-Path $output 'dependents.txt')
$stream = [IO.File]::OpenRead($dll)
try {
    $reader = New-Object IO.BinaryReader($stream)
    $stream.Position = 0x3c; $stream.Position = $reader.ReadInt32()
    if ($reader.ReadUInt32() -ne 0x4550) { throw 'Invalid PE signature' }
    $expected = @{arm64=0xaa64;x64=0x8664;x86=0x14c}[$Architecture]
    if ($reader.ReadUInt16() -ne $expected) { throw 'UMD PE architecture mismatch' }
} finally { $stream.Dispose() }
[PSCustomObject]@{Source=(& git rev-parse HEAD); Submodules=(& git submodule status --recursive);
    Architecture=$Architecture; WindowsKit=$sdkVersion; NativeRuntimeValidated=$false;
    Contract='partial native DDI bridge; no OpenAdapter12; no app-local D3D12 replacement'} |
    ConvertTo-Json -Depth 4 | Set-Content (Join-Path $output 'source.json')
$hashes = Get-ChildItem $output -File | Get-FileHash -Algorithm SHA256
$hashes | ForEach-Object { $_.Hash + '  ' + [IO.Path]::GetFileName($_.Path) } |
    Set-Content (Join-Path $output 'SHA256SUMS')
