# report_mod\tests\run_report.ps1 [-Work <folder>]
#
# The tests of "Report a bug": the pure logic and the gathering on a fake game folder (report_test), the picture on a D3D12 swap chain of its own
# (report_capture_test, on Windows' software adapter: no GPU, no game). Nothing here reaches the network. A whole report against a local server:
# report_e2e_test.exe, which needs a server on this machine named on its command line and refuses any other.
# Smart App Control may block a fresh test exe (its first verdict is not final): run it again, or rebuild.
param(
    [string] $Work
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $Work) { $Work = Join-Path $repo 'build\report_tests\work_report' }
& cmd /d /c "`"$PSScriptRoot\build_report_tests.bat`""
if ($LASTEXITCODE -ne 0) { throw "build_report_tests failed" }
$bin = Join-Path $repo 'build\report_tests'
New-Item -ItemType Directory -Force $Work | Out-Null
& (Join-Path $bin 'report_test.exe') --work $Work --repo $repo
$a = $LASTEXITCODE
& (Join-Path $bin 'report_capture_test.exe')
$b = $LASTEXITCODE
Write-Host ("report_test: " + $(if ($a -eq 0) { 'green' } else { 'FAILED' }) + "; report_capture_test: " + $(if ($b -eq 0) { 'green' } else { 'FAILED' }))
if ($a -ne 0 -or $b -ne 0) { exit 1 }
exit 0
