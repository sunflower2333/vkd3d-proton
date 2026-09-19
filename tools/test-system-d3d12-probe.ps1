param([Parameter(Mandatory)][string]$Executable)
$ErrorActionPreference = 'Stop'
$exe = (Resolve-Path -LiteralPath $Executable).Path
$directory = Split-Path -Parent $exe
$stdout = Join-Path $directory 'system-d3d12-warp.stdout.txt'
$stderr = Join-Path $directory 'system-d3d12-warp.stderr.txt'
$process = Start-Process -FilePath $exe -ArgumentList '--run-warp-ci' -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
$null = $process.Handle
if (!$process.WaitForExit(60000)) { $process.Kill(); $process.WaitForExit(); throw 'Public D3D12 WARP harness exceeded 60 seconds' }
$output = Get-Content -LiteralPath $stdout -Raw
$errors = Get-Content -LiteralPath $stderr -Raw
Write-Host $output
if ($process.ExitCode -ne 0 -or $errors -match 'FAIL|MISMATCH|NOT_PASSED' -or
        $output -notmatch 'PASS PUBLIC_FENCE_LIFECYCLE delayed_queue_completion, CPU_rewind, final_release_event' -or
        $output -notmatch 'PASS CPU_WARP_HARNESS_ONLY 4x1024 words; no VIOGPU acceptance') {
    throw "Public D3D12 application harness failed: exit=$($process.ExitCode) $errors"
}
