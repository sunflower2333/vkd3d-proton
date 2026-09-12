param([Parameter(Mandatory)][string]$Executable)
$ErrorActionPreference = 'Stop'
$exe = (Resolve-Path -LiteralPath $Executable).Path
$directory = Split-Path -Parent $exe
foreach ($scenario in @('lifecycle', 'deferred-negative', 'command-owner-negative')) {
    $negative = $scenario -ne 'lifecycle'
    $name = 'runtime-' + $scenario
    $stdout = Join-Path $directory ($name + '.stdout.txt')
    $stderr = Join-Path $directory ($name + '.stderr.txt')
    $start = @{ FilePath = $exe; PassThru = $true; RedirectStandardOutput = $stdout; RedirectStandardError = $stderr }
    if ($scenario -eq 'deferred-negative') { $start.ArgumentList = '--negative-control-deferred-backend' }
    if ($scenario -eq 'command-owner-negative') { $start.ArgumentList = '--negative-control-command-error-owner' }
    $process = Start-Process @start
    $null = $process.Handle
    if (!$process.WaitForExit(90000)) {
        $process.Kill(); $process.WaitForExit()
        throw "$name exceeded 90 seconds"
    }
    $output = Get-Content -LiteralPath $stdout -Raw
    $errors = Get-Content -LiteralPath $stderr -Raw
    if ($negative) {
        $expected = if ($scenario -eq 'deferred-negative') {
            'FAIL ordinary backend completion after premature callback retirement'
        } else { 'FAIL native command error escaped its runtime command-list owner' }
        if ($process.ExitCode -ne 1 -or $errors -notmatch $expected) {
            throw "$scenario negative control did not fail semantically: exit=$($process.ExitCode) $errors"
        }
        Write-Host "PASS negative control ${scenario}: $expected"
    } else {
        Write-Host $output
        if ($process.ExitCode -ne 0 -or $errors -match 'FAIL' -or
                $output -notmatch 'PASS ordinary backend completion/unmap/release before runtime retirement' -or
                $output -notmatch 'PASS native command-list runtime error ownership, device-loss forwarding, rejected creation and reentrant retirement') {
            throw "Runtime lifecycle fixture failed: exit=$($process.ExitCode) $errors"
        }
    }
}
