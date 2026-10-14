# crsm\tests\run_community.ps1 [-Work <folder>]
#
# The tests of the Community client (phase P2, 2.0.1's fixes, 2.1.0's Steam-verified identity): requests / answers /
# identity offline, a stub HTTP server in this process for the wire and the failures, a fake steam_api for the Steam
# engine. Nothing here reaches the Community Worker or Steam.
# Builds build\crsm_tests\community_test.exe, exits 0 only when every check passed. Two checks measure real time (the
# 600-tick timeout and the watchdog): on a machine whose timer is coarse or busy they can fail without a code cause.
param(
    [string] $Work
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $Work) { $Work = Join-Path $repo 'build\crsm_tests\work_community' }
& cmd /d /c "`"$PSScriptRoot\build_community_tests.bat`""
if ($LASTEXITCODE -ne 0) { throw "build_community_tests failed" }
$bin = Join-Path $repo 'build\crsm_tests'
New-Item -ItemType Directory -Force $Work | Out-Null
& (Join-Path $bin 'community_test.exe') --work $Work
exit $LASTEXITCODE
