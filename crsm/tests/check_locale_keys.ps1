# crsm\tests\check_locale_keys.ps1
#
# Every text key the C++ of the Recording Manager asks for is in the `en` block of data\SF6_CRSM_data\locale.json
# (ui::lang::T returns the bare key when a text is missing, so a missing key would show on screen).
# Looks for the keys in the calls T("x"), T_("x"), tr("x"), tr_or("x", ...), words("x", ...), Tr-like lambdas, the Msg
# literals of community\page.cpp (the com_* keys), the status keys of ui\manager_logic.cpp (st_*), the hints' keys and
# the fighter.<id> names of model\chars.cpp. Also lists the keys of `en` that no C++ names (information; dead keys
# are not an error), and checks that no language block has a key the en block lacks.
# Exits 0 when nothing is missing.
$ErrorActionPreference = 'Stop'
$crsm = Split-Path -Parent $PSScriptRoot
$json = Get-Content -Raw -Encoding UTF8 (Join-Path $crsm 'data\SF6_CRSM_data\locale.json') | ConvertFrom-Json
$en = @{}
foreach ($p in $json.en.PSObject.Properties) { $en[$p.Name] = $true }

$used = @{}
$files = Get-ChildItem -Recurse -Include *.cpp, *.hpp (Join-Path $crsm 'src')
$call = [regex]'\b(?:T|T_|tr|tr_or|words|lang::T)\(\s*"([a-z][a-z0-9_]*)"'
foreach ($f in $files) {
    $text = Get-Content -Raw -Encoding UTF8 $f.FullName
    foreach ($m in $call.Matches($text)) { $used["rsm." + $m.Groups[1].Value] = $f.Name }
    if ($f.Name -eq 'page.cpp') {
        foreach ($m in [regex]::Matches($text, '"(com_[a-z0-9_]+)"')) { $used["rsm." + $m.Groups[1].Value] = $f.Name }
    }
    if ($f.Name -eq 'manager_logic.cpp') {
        foreach ($m in [regex]::Matches($text, '"((?:st_|file_)[a-z0-9_]+)"')) { $used["rsm." + $m.Groups[1].Value] = $f.Name }
        foreach ($m in [regex]::Matches($text, 'kButton[XY], "([a-z_]+)"')) { $used["rsm." + $m.Groups[1].Value] = $f.Name }
    }
}
$chars = Get-Content -Raw -Encoding UTF8 (Join-Path $crsm 'src\model\chars.cpp')
foreach ($m in [regex]::Matches($chars, '\{\s*(\d+),\s*"')) { $used["fighter." + $m.Groups[1].Value] = 'chars.cpp' }

$missing = @($used.Keys | Where-Object { -not $en.ContainsKey($_) } | Sort-Object)
foreach ($k in $missing) { Write-Host ("MISSING in en: " + $k + "  (" + $used[$k] + ")") }

$extra = @()
foreach ($lang in $json.PSObject.Properties) {
    if ($lang.Name -eq '_readme' -or $lang.Name -eq 'en') { continue }
    foreach ($p in $lang.Value.PSObject.Properties) { if (-not $en.ContainsKey($p.Name)) { $extra += ($lang.Name + ": " + $p.Name) } }
}
foreach ($e in $extra) { Write-Host ("not in en: " + $e) }

$unused = @($en.Keys | Where-Object { -not $used.ContainsKey($_) } | Sort-Object)
Write-Host ("keys used by the C++: " + $used.Count + "; en block: " + $en.Count + "; missing: " + $missing.Count + "; in a language and not in en: " + $extra.Count)
if ($unused.Count) { Write-Host ("en keys no literal names (read through a variable, or dead): " + ($unused -join ', ')) }
if ($missing.Count -or $extra.Count) { exit 1 }
exit 0
