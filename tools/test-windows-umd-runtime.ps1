param([Parameter(Mandatory)][string]$Executable)
$ErrorActionPreference = 'Stop'
$exe = (Resolve-Path -LiteralPath $Executable).Path
$directory = Split-Path -Parent $exe
foreach ($scenario in @('lifecycle', 'deferred-negative', 'command-owner-negative', 'uav-global-negative', 'queue-owner-negative', 'completion-negative', 'indirect-count-negative', 'fence-owner-negative', 'fence-mask-negative')) {
    $negative = $scenario -ne 'lifecycle'
    $name = 'runtime-' + $scenario
    $stdout = Join-Path $directory ($name + '.stdout.txt')
    $stderr = Join-Path $directory ($name + '.stderr.txt')
    $start = @{ FilePath = $exe; PassThru = $true; RedirectStandardOutput = $stdout; RedirectStandardError = $stderr }
    if ($scenario -eq 'deferred-negative') { $start.ArgumentList = '--negative-control-deferred-backend' }
    if ($scenario -eq 'command-owner-negative') { $start.ArgumentList = '--negative-control-command-error-owner' }
    if ($scenario -eq 'uav-global-negative') { $start.ArgumentList = '--negative-control-global-uav-barrier' }
    if ($scenario -eq 'queue-owner-negative') { $start.ArgumentList = '--negative-control-queue-ownership' }
    if ($scenario -eq 'completion-negative') { $start.ArgumentList = '--negative-control-os-completion' }
    if ($scenario -eq 'indirect-count-negative') { $start.ArgumentList = '--negative-control-indirect-count' }
    if ($scenario -eq 'fence-owner-negative') { $start.ArgumentList = '--negative-control-native-fence-owner' }
    if ($scenario -eq 'fence-mask-negative') { $start.ArgumentList = '--negative-control-native-fence-mask' }
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
        } elseif ($scenario -eq 'uav-global-negative') {
            'FAIL native UAV barrier lost its resource/global ordering'
        } elseif ($scenario -eq 'queue-owner-negative') {
            'FAIL native queue execution released a submitted backend owner'
        } elseif ($scenario -eq 'completion-negative') {
            'FAIL native DMA ownership retired before OS completion event'
        } elseif ($scenario -eq 'indirect-count-negative') {
            'FAIL native indirect dispatch lost GPU count or buffer placement'
        } elseif ($scenario -eq 'fence-owner-negative') {
            'FAIL native fence accepted foreign or stale runtime ownership'
        } elseif ($scenario -eq 'fence-mask-negative') {
            'FAIL native fence omitted single-node runtime broadcast mask'
        } else { 'FAIL native command error escaped its runtime command-list owner' }
        if ($process.ExitCode -ne 1 -or $errors -notmatch $expected) {
            throw "$scenario negative control did not fail semantically: exit=$($process.ExitCode) $errors"
        }
        Write-Host "PASS negative control ${scenario}: $expected"
    } else {
        Write-Host $output
        if ($process.ExitCode -ne 0 -or $errors -match 'FAIL' -or
                $output -notmatch 'PASS ordinary backend completion/unmap/release before runtime retirement' -or
                $output -notmatch 'PASS native texture heap allocation/import, request poisoning, format ownership, alias lifetime and failure cleanup' -or
                $output -notmatch 'PASS native RT shared allocation/import/placement and retained alias backing' -or
                $output -notmatch 'PASS native allocation-info query, backend requirements, no allocation, rejected-output preservation and retirement' -or
                $output -notmatch 'PASS native UAV resource/global barriers, whole-batch rejection and command error ownership' -or
                $output -notmatch 'PASS native queue identity, whole-batch ownership, reentrant execution retirement and constructor cancellation' -or
                $output -notmatch 'PASS native OS completion events, ordered DMA ownership, bounded pending retirement and callback cancellation' -or
                $output -notmatch 'PASS native indirect signatures, GPU argument/count forwarding, rejected handles and reentrant ownership' -or
                $output -notmatch 'PASS native single-node fence copies, runtime broadcast selection, foreign/stale rejection and reentrant callback retirement' -or
                $output -notmatch 'PASS native command-list runtime error ownership, device-loss forwarding, rejected creation and reentrant retirement') {
            throw "Runtime lifecycle fixture failed: exit=$($process.ExitCode) $errors"
        }
    }
}
