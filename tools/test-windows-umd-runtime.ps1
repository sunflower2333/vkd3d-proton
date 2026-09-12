param([Parameter(Mandatory)][string]$Executable)
$ErrorActionPreference = 'Stop'
$exe = (Resolve-Path -LiteralPath $Executable).Path
$directory = Split-Path -Parent $exe
foreach ($negative in @($false, $true)) {
    $name = if ($negative) { 'runtime-deferred-negative' } else { 'runtime-lifecycle' }
    $stdout = Join-Path $directory ($name + '.stdout.txt')
    $stderr = Join-Path $directory ($name + '.stderr.txt')
    $start = @{ FilePath = $exe; PassThru = $true; RedirectStandardOutput = $stdout; RedirectStandardError = $stderr }
    if ($negative) { $start.ArgumentList = '--negative-control-deferred-backend' }
    $process = Start-Process @start
    $null = $process.Handle
    if (!$process.WaitForExit(90000)) {
        $process.Kill(); $process.WaitForExit()
        throw "$name exceeded 90 seconds"
    }
    $output = Get-Content -LiteralPath $stdout -Raw
    $errors = Get-Content -LiteralPath $stderr -Raw
    if ($negative) {
        if ($process.ExitCode -ne 1 -or $errors -notmatch 'FAIL ordinary backend completion after premature callback retirement') {
            throw "Deferred-backend negative control did not fail semantically: exit=$($process.ExitCode) $errors"
        }
        Write-Host 'PASS negative control: ordinary destructor oracle rejects retired completion callbacks'
    } else {
        Write-Host $output
        if ($process.ExitCode -ne 0 -or $errors -match 'FAIL' -or
                $output -notmatch 'PASS ordinary backend completion/unmap/release before runtime retirement') {
            throw "Runtime lifecycle fixture failed: exit=$($process.ExitCode) $errors"
        }
    }
}
