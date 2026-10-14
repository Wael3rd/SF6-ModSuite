# crsm\tests\run.ps1 [-Work <folder>]
#
# The offline benches of the Recording Manager: no game, no user data, nothing outside -Work is written.
#   -Work     scratch folder for the benches' files (default build\crsm_tests\work)
#
# Builds build\crsm_tests\p3_bench.exe, ui_bench.exe, p5_bench.exe, integ_bench.exe and codecache_bench.exe, runs them,
# exits 0 only when every check passed. The Community client's tests: run_community.ps1.
param(
    [string] $Work
)
$ErrorActionPreference = 'Stop'
$crsm = Split-Path -Parent $PSScriptRoot
$repo = Split-Path -Parent $crsm
if (-not $Work) { $Work = Join-Path $repo 'build\crsm_tests\work' }

& cmd /d /c "`"$PSScriptRoot\build_tests.bat`""
if ($LASTEXITCODE -ne 0) { throw "build failed" }

$bin = Join-Path $repo 'build\crsm_tests'
New-Item -ItemType Directory -Force $Work | Out-Null
& (Join-Path $bin 'p3_bench.exe') --work $Work               # P3: files (name cut), replay / reversal / record pure, slot 8 shared
$p3 = $LASTEXITCODE
& (Join-Path $bin 'ui_bench.exe')                            # P4: the pure logic of the DLL's screens
$p4 = $LASTEXITCODE
& (Join-Path $bin 'p5_bench.exe') --repo $crsm --work $Work  # P5: the Manager page's logic (the shipped locale.json)
$p5 = $LASTEXITCODE
& (Join-Path $bin 'integ_bench.exe') --repo $crsm            # P4 + P5 integration: owners, keys, tickers, refresh, settings.json
$pi = $LASTEXITCODE
& (Join-Path $bin 'codecache_bench.exe')                     # the code scan's cache
$pc = $LASTEXITCODE
Write-Host ("P3 bench: " + $(if ($p3 -eq 0) { 'green' } else { 'FAILED' }) + "; UI bench (P4): " + $(if ($p4 -eq 0) { 'green' } else { 'FAILED' }) + "; P5 bench: " + $(if ($p5 -eq 0) { 'green' } else { 'FAILED' }) + "; integration bench: " + $(if ($pi -eq 0) { 'green' } else { 'FAILED' }) + "; code scan cache: " + $(if ($pc -eq 0) { 'green' } else { 'FAILED' }))
if ($p3 -ne 0 -or $p4 -ne 0 -or $p5 -ne 0 -or $pi -ne 0 -or $pc -ne 0) { exit 1 }
exit 0
