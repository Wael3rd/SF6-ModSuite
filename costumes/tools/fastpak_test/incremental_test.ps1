# Incremental costume pak (IncrementalPak) against a full build, offline.
#
# For each scenario: a full build makes the "previous" pak and its pak_sources.tsv; the scenario changes
# the mods; an incremental build (IncrementalPak=on) and a full build (IncrementalPak=off) then start from
# the same registry state, and their paks must be byte for byte the same (pak_manifest.py, whole file),
# as must the files written beside them. Every change is undone at the end and the lab's data folder is
# put back as it was; the lab's pak is the last one built.
#
#   powershell -File incremental_test.ps1 -Exe <costume_loader.exe> -Lab <game folder copy> -BasePak <re_chunk_000.pak>
#       -Work <empty folder> [-TouchFile <archive>] [-ByteFile <loose mod file>] [-RemoveFile <archive>]
#
# Scenarios: none (nothing changed, the whole pak taken from the previous one), touch (an archive's date
# changes: extracted again, read again), byte (a byte of a loose mod file changes, with its date), remove
# (an archive taken out: the slots after it move), byte_on (byte with RuntimeOptions=on).
# Needs about 3 x the pak's size free on the lab's disk. Exit code 0 when every scenario is identical.
param(
    [Parameter(Mandatory)] [string]$Exe,
    [Parameter(Mandatory)] [string]$Lab,
    [Parameter(Mandatory)] [string]$BasePak,
    [Parameter(Mandatory)] [string]$Work,
    [string]$TouchFile = "",
    [string]$ByteFile = "",
    [string]$RemoveFile = "",
    [string[]]$Scenarios = @("none", "touch", "byte", "remove", "byte_on")
)
$ErrorActionPreference = "Stop"
$manifest = Join-Path $PSScriptRoot "pak_manifest.py"
$data = Join-Path $Lab "reframework\data\SF6_Costumes_Data"
$ini = Join-Path $Lab "reframework\costume_mods\settings.ini"
$log = Join-Path $Lab "SF6_CostumeLoader.log"
New-Item -ItemType Directory -Force $Work | Out-Null

function Save-State([string]$name) {
    $d = Join-Path $Work "state_$name"
    if (Test-Path $d) { Remove-Item -Recurse -Force $d }
    New-Item -ItemType Directory $d | Out-Null
    Get-ChildItem $data -File | Where-Object { $_.Extension -in ".json", ".tsv" } | Copy-Item -Destination $d
}
function Restore-State([string]$name) {
    Get-ChildItem $data -File | Where-Object { $_.Extension -in ".json", ".tsv" } | Remove-Item -Force
    Copy-Item (Join-Path $Work "state_$name\*") $data -Force
}
function Clear-Fingerprint {
    $reg = Join-Path $data "registry.json"
    if (Test-Path $reg) {
        $t = [IO.File]::ReadAllText($reg) -replace '"fingerprint": "[^"]*"', '"fingerprint": ""'
        [IO.File]::WriteAllText($reg, $t)
    }
}
function Our-Pak {
    Get-ChildItem $Lab -Filter "re_chunk_000.pak.patch_*.pak" | Sort-Object Name | Select-Object -Last 1
}
function Build([string]$mode, [string]$incremental, [string]$tag) {
    $free = (Get-PSDrive ($Lab.Substring(0, 1))).Free / 1GB
    if ($free -lt 30) { throw ("only {0:N1} GB free" -f $free) }
    Clear-Fingerprint
    $old = if (Test-Path $ini) { [IO.File]::ReadAllText($ini) } else { $null }
    [IO.File]::WriteAllText($ini, "RuntimeOptions=$mode`r`nIncrementalPak=$incremental`r`n")
    Remove-Item $log -ErrorAction SilentlyContinue
    $sw = [Diagnostics.Stopwatch]::StartNew()
    & $Exe $Lab --base-pak $BasePak > (Join-Path $Work "out_$tag.txt")
    $code = $LASTEXITCODE
    $sw.Stop()
    if ($null -eq $old) { Remove-Item $ini } else { [IO.File]::WriteAllText($ini, $old) }
    Copy-Item $log (Join-Path $Work "log_$tag.txt")
    if ($code -ne 0) { throw "build $tag failed ($code)" }
    $pak = Our-Pak
    & python $manifest $pak.FullName -o (Join-Path $Work "man_$tag.txt") | Out-Null
    $reused = (Select-String -Path $log -Pattern "entries taken from the previous pak" | Select-Object -First 1)
    [PSCustomObject]@{ Seconds = [Math]::Round($sw.Elapsed.TotalSeconds, 1)
                       Reused = if ($reused) { ($reused.Line -replace '^\[[^\]]*\]\s*', '') } else { "-" } }
}
function Same-Json([string]$a, [string]$b) {
    $ok = $true
    foreach ($f in Get-ChildItem $a -Filter *.json) {
        $o = Join-Path $b $f.Name
        if (-not (Test-Path $o)) { $ok = $false; continue }
        $x = (Get-Content $f.FullName) -notmatch '"(generated|fingerprint)"'
        $y = (Get-Content $o) -notmatch '"(generated|fingerprint)"'
        if (Compare-Object $x $y) { Write-Output "    $($f.Name) differs"; $ok = $false }
    }
    $ok
}

$undo = @()
$results = @()
Save-State "start"
try {
    foreach ($sc in $Scenarios) {
        $mode = if ($sc -like "*_on") { "on" } else { "off" }
        $kind = $sc -replace "_on$", ""
        if ($kind -eq "touch" -and -not $TouchFile) { continue }
        if ($kind -eq "byte" -and -not $ByteFile) { continue }
        if ($kind -eq "remove" -and -not $RemoveFile) { continue }
        Write-Output "== $sc"
        Restore-State "start"
        $prev = Build $mode "off" "${sc}_prev"
        Save-State "${sc}_prev"
        # the change
        switch ($kind) {
            "touch" {
                $t0 = (Get-Item $TouchFile).LastWriteTime
                $undo += { (Get-Item $TouchFile).LastWriteTime = $t0 }.GetNewClosure()
                (Get-Item $TouchFile).LastWriteTime = (Get-Date)
            }
            "byte" {
                $item = Get-Item $ByteFile
                $t0 = $item.LastWriteTime
                $bytes = [IO.File]::ReadAllBytes($ByteFile)
                $at = $bytes.Length - 17
                $b0 = $bytes[$at]
                $undo += { $x = [IO.File]::ReadAllBytes($ByteFile); $x[$at] = $b0; [IO.File]::WriteAllBytes($ByteFile, $x); (Get-Item $ByteFile).LastWriteTime = $t0 }.GetNewClosure()
                $bytes[$at] = $b0 -bxor 0x5A
                [IO.File]::WriteAllBytes($ByteFile, $bytes)
                (Get-Item $ByteFile).LastWriteTime = (Get-Date)
            }
            "remove" {
                $aside = Join-Path $Work ("aside_" + (Split-Path $RemoveFile -Leaf))
                Move-Item $RemoveFile $aside
                $undo += { Move-Item $aside $RemoveFile }.GetNewClosure()
            }
        }
        $inc = Build $mode "on" "${sc}_inc"
        Save-State "${sc}_inc"
        Restore-State "${sc}_prev"
        $full = Build $mode "off" "${sc}_full"
        Save-State "${sc}_full"
        # undo the change before comparing (the next scenario starts from the lab as it was)
        for ($i = $undo.Count - 1; $i -ge 0; $i--) { & $undo[$i] }
        $undo = @()
        $cmp = & python $manifest --compare (Join-Path $Work "man_${sc}_full.txt") (Join-Path $Work "man_${sc}_inc.txt")
        $pakSame = $LASTEXITCODE -eq 0
        $cmp | Select-Object -First 2 | ForEach-Object { "    $_" }
        $jsonSame = Same-Json (Join-Path $Work "state_${sc}_full") (Join-Path $Work "state_${sc}_inc")
        $results += [PSCustomObject]@{ Scenario = $sc; PakIdentical = $pakSame; FilesIdentical = ($jsonSame | Select-Object -Last 1)
                                       PrevSec = $prev.Seconds; IncSec = $inc.Seconds; FullSec = $full.Seconds; Reused = $inc.Reused }
    }
} finally {
    for ($i = $undo.Count - 1; $i -ge 0; $i--) { & $undo[$i] }
    Restore-State "start"
}
$results | Format-Table -AutoSize | Out-String -Width 250
if ($results | Where-Object { -not $_.PakIdentical -or -not $_.FilesIdentical }) { exit 1 }
exit 0
