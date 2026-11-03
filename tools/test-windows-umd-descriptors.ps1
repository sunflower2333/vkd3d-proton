param([Parameter(Mandatory=$true)][string]$Executable)
$ErrorActionPreference = 'Stop'
$exe = (Resolve-Path -LiteralPath $Executable).Path
foreach ($negative in @($false, $true)) {
    $name = if ($negative) { 'descriptor-vertex-negative' } else { 'descriptor-validation' }
    $stdout = Join-Path (Split-Path $exe) "$name.stdout.txt"
    $stderr = Join-Path (Split-Path $exe) "$name.stderr.txt"
    $start = @{ FilePath=$exe; PassThru=$true; NoNewWindow=$true; RedirectStandardOutput=$stdout; RedirectStandardError=$stderr }
    if ($negative) { $start.ArgumentList = '--negative-control-vertex-field-order' }
    $process = Start-Process @start
    $null = $process.Handle
    if (!$process.WaitForExit(90000)) {
        $process.Kill(); $process.WaitForExit()
        throw "$name exceeded 90 seconds"
    }
    $output = Get-Content -LiteralPath $stdout -Raw
    $errors = Get-Content -LiteralPath $stderr -Raw
    if ($negative) {
        if ($process.ExitCode -ne 1 -or $errors -notmatch 'FAIL native vertex CORE_0003 field order') {
            throw "Vertex order negative control did not fail semantically: exit=$($process.ExitCode) $errors"
        }
        Write-Host 'PASS negative control: native vertex CORE_0003 field order'
    } else {
        Write-Host $output
        if ($process.ExitCode -ne 0 -or $errors -match 'FAIL' -or
                $output -notmatch 'PASS native vertex buffers: CORE_0003 order') {
            throw "Native descriptor validation failed: exit=$($process.ExitCode) $errors"
        }
    }
}
