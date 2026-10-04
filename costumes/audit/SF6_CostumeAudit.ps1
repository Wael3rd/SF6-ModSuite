# SF6 Costume Slots Loader - audit
# Lists the costume mods installed, where they come from (Fluffy Mod Manager pak, archive dropped in
# costume_mods, unpacked folder), the outfits the loader made of them, and what the last launch reported.
# Reads files only; changes nothing. Windows PowerShell 5.1 or newer.

$ErrorActionPreference = 'SilentlyContinue'

# ---- game folder: this script sits in reframework\costume_mods, or in the game folder itself ----
$game = $null
$dir = $PSScriptRoot
for ($i = 0; $i -lt 4 -and $dir; $i++) {
    if (Test-Path (Join-Path $dir 're_chunk_000.pak')) { $game = $dir; break }
    $dir = Split-Path $dir -Parent
}
if (-not $game) {
    Write-Host 'Street Fighter 6 folder not found. Put this script in reframework\costume_mods or in the game folder.'
    exit 1
}
$ref = Join-Path $game 'reframework'
$mods = Join-Path $ref 'costume_mods'
$data = Join-Path $ref 'data\SF6_Costumes_Data'

$names = @{ 1='Ryu'; 2='Luke'; 3='Kimberly'; 4='Chun-Li'; 5='Manon'; 6='Zangief'; 7='JP'; 8='Dhalsim';
            9='Cammy'; 10='Ken'; 11='Dee Jay'; 12='Lily'; 13='A.K.I.'; 14='Rashid'; 15='Blanka'; 16='Juri';
            17='Marisa'; 18='Guile'; 19='Ed'; 20='E. Honda'; 21='Jamie'; 22='Akuma'; 25='Sagat'; 26='M. Bison';
            27='Terry'; 28='Mai'; 29='Elena'; 30='C. Viper'; 31='Alex'; 32='Ingrid'; 33='Yasmine' }
$archiveExt = @('.zip', '.7z', '.rar')

$out = New-Object System.Collections.Generic.List[string]
function Say([string]$s) { $out.Add($s) }

Say 'SF6 Costume Slots Loader - audit'
Say ('Generated ' + (Get-Date -Format 'yyyy-MM-dd HH:mm'))
Say ('Game folder: ' + $game)
Say ''

# ---- installation ----
Say '== Installation =='
function Check([string]$label, [bool]$ok, [string]$hint) {
    if ($ok) { Say ('  [OK]      ' + $label) } else { Say ('  [MISSING] ' + $label + '  -> ' + $hint) }
}
$proxy = Join-Path $game 'amd_ags_x64.dll'
$isProxy = $false
if (Test-Path $proxy) {
    $bytes = [IO.File]::ReadAllBytes($proxy)
    $isProxy = [Text.Encoding]::ASCII.GetString($bytes).Contains('amd_ags_x64_real')
}
Check 'Loader (amd_ags_x64.dll)' $isProxy 'install the Costume Slots Loader archive'
Check 'Original AMD library (amd_ags_x64_real.dll)' (Test-Path (Join-Path $game 'amd_ags_x64_real.dll')) 'reinstall the loader archive'
Check 'REFramework (dinput8.dll)' (Test-Path (Join-Path $game 'dinput8.dll')) 'install REFramework 1.5.8 or newer'
Check 'Native plugin (reframework\plugins\SF6_CostumeSlotsNative.dll)' (Test-Path (Join-Path $ref 'plugins\SF6_CostumeSlotsNative.dll')) 'reinstall the loader archive (menus, colours, online)'
if (Test-Path (Join-Path $ref 'autorun\SF6_CostumeSlots.lua')) { Say '  [INFO]    autorun\SF6_CostumeSlots.lua is left from a version before 1.9: the plugin renames it at the next launch' }
Check 'Loader tables (reframework\data\SF6_Costumes_Data\loader)' (Test-Path (Join-Path $data 'loader\static\static_meta.json')) 'reinstall the loader archive'
$chain = Join-Path $game 'amd_ags_x64_chain.dll'
if (Test-Path $chain) { Say '  [INFO]    amd_ags_x64_chain.dll present: another tool is chained behind the loader' }
Say ''

# ---- last launch ----
Say '== Last launch =='
$logPath = Join-Path $game 'SF6_CostumeLoader.log'
if (Test-Path $logPath) {
    $log = [IO.File]::ReadAllText($logPath, [Text.Encoding]::Default)
    $start = $log.LastIndexOf('=== SF6 Costume Slot Loader')
    $start = $log.LastIndexOf("`n", [Math]::Max(0, $start)) + 1
    $last = $log.Substring([Math]::Max(0, $start))
    $firstLine = ($last -split "`n")[0]
    $when = ''
    if ($firstLine -match '^\[([^\]]+)\]') { $when = $Matches[1] }
    if ($last -match 'Up to date') { Say ('  ' + $when + ': nothing changed since the previous launch, outfits kept') }
    elseif ($last -match 'Done in ([\d\.]+) ms') { Say ('  ' + $when + (': outfits rebuilt in {0:N0} s' -f ([double]$Matches[1] / 1000))) }
    else { Say ('  ' + $when + ': the loader did not finish (see SF6_CostumeLoader.log)') }
    # warnings of the last generation (the last run that rebuilt anything)
    $gen = $log.LastIndexOf('Assigning slots')
    if ($gen -ge 0) {
        $genStart = $log.LastIndexOf('=== SF6 Costume Slot Loader', $gen)
        $genText = $log.Substring([Math]::Max(0, $genStart))
        $warn = ($genText -split "`n") | Where-Object { $_ -match 'WARN|ERROR' } | ForEach-Object { ($_ -replace '^\[[^\]]+\]\s*', '').Trim() }
        $fixed = (($genText -split "`n") | Where-Object { $_ -match 'check:|repaired|brought to the current|converted from the 2023' }).Count
        if ($fixed) { Say ('  ' + $fixed + ' repair(s) made automatically on mods made for older versions of the game') }
        foreach ($w in $warn) { Say ('  ' + $w) }
    }
    foreach ($e in (($last -split "`n") | Where-Object { $_ -match 'ERROR' })) { Say ('  ' + (($e -replace '^\[[^\]]+\]\s*', '').Trim())) }
} else {
    Say '  No SF6_CostumeLoader.log: the game has not been started with the loader yet.'
}
Say ''

# ---- outfits made by the last generation ----
$list = $null
$listPath = Join-Path $data 'outfits.json'
if (Test-Path $listPath) { $list = [IO.File]::ReadAllText($listPath, [Text.Encoding]::UTF8) | ConvertFrom-Json }
$bySource = @{}
if ($list) {
    foreach ($o in $list.outfits) {
        $k = ([string]$o.source).ToLower()
        if (-not $bySource.ContainsKey($k)) { $bySource[$k] = New-Object System.Collections.Generic.List[object] }
        $bySource[$k].Add($o)
    }
}
function OutfitsOf([string]$source) {
    $k = $source.ToLower()
    if ($bySource.ContainsKey($k)) { return $bySource[$k] }
    return $null
}
function BasedOn($o) {
    if ([int]$o.based_on -eq 4) { return 'based on Drive Tech' }
    return ('based on Outfit ' + ([int]$o.based_on + 1))
}
function Describe($o) {
    $who = $names[[int]$o.fighter]; if (-not $who) { $who = $o.fighter_dir }
    return ($who + ' ' + $o.name + ' (' + (BasedOn $o) + ')')
}

# ---- costume mods found on disk ----
Say '== Costume mods found =='
$ourPak = ''
if ($list) { $ourPak = [string]$list.pak }
$paks = Get-ChildItem $game -Filter 're_chunk_000.pak.patch_*.pak' | Sort-Object Name
$fluffy = $paks | Where-Object { $_.Name -ne $ourPak }
Say '  Fluffy Mod Manager (patch paks in the game folder):'
if (-not $fluffy) { Say '    none' }
foreach ($p in $fluffy) {
    $os = OutfitsOf $p.Name
    if ($os) { foreach ($o in $os) { Say ('    ' + $p.Name + '  ->  ' + (Describe $o)) } }
    else { Say ('    ' + $p.Name + '  ->  no outfit (not a costume mod, or installed after the last launch)') }
}
if ($ourPak) { Say ('    (' + $ourPak + ' is the one the loader builds itself)') }
Say '  Fluffy Mod Manager (mods ticked in Fluffy, read from their archives):'
$fl = @()
if ($list) { $fl = @($list.outfits | Where-Object { ([string]$_.source).StartsWith('Fluffy Mod Manager/') }) }
if (-not $fl) { Say '    none' }
foreach ($g in ($fl | Group-Object source)) {
    Say ('    ' + $g.Name.Substring('Fluffy Mod Manager/'.Length))
    foreach ($o in $g.Group) { Say ('        -> ' + (Describe $o) + ': ' + $o.label) }
}

Say '  reframework\costume_mods:'
$found = 0
$entries = New-Object System.Collections.Generic.List[object]
if (Test-Path $mods) {
    foreach ($f in (Get-ChildItem $mods -File | Where-Object { $archiveExt -contains $_.Extension.ToLower() })) {
        $entries.Add(@{ rel = $f.Name; kind = 'archive' })
    }
    foreach ($c in (Get-ChildItem $mods -Directory | Where-Object { -not $_.Name.StartsWith('.') } | Sort-Object Name)) {
        foreach ($e in (Get-ChildItem $c.FullName | Where-Object { -not $_.Name.StartsWith('.') } | Sort-Object Name)) {
            $kind = $null
            if ($e.PSIsContainer) { $kind = 'folder' }
            elseif ($archiveExt -contains $e.Extension.ToLower()) { $kind = 'archive' }
            elseif ($e.Extension.ToLower() -eq '.pak') { $kind = 'pak' }
            if ($kind) { $entries.Add(@{ rel = ($c.Name + '/' + $e.Name); kind = $kind }) }
        }
    }
}
foreach ($en in $entries) {
    $found++
    $label = switch ($en.kind) { 'archive' { 'archive as downloaded' } 'folder' { 'folder' } 'pak' { 'pak file' } }
    $os = OutfitsOf $en.rel
    Say ('    ' + ($en.rel -replace '/', '\') + '   [' + $label + ']')
    if ($os) { foreach ($o in $os) { Say ('        -> ' + (Describe $o) + ': ' + $o.label) } }
    else { Say '        -> no outfit yet: start the game once (or see SF6_CostumeLoader.log if it stays so)' }
}
if (-not $found) { Say '    none' }
Say ''

# ---- outfits by character ----
Say '== Extra outfits in game, by character =='
if (-not $list) {
    Say '  No list yet: start the game once with the loader installed.'
} else {
    $total = 0
    foreach ($g in ($list.outfits | Group-Object fighter | Sort-Object { [int]$_.Name })) {
        $who = $names[[int]$g.Name]; if (-not $who) { $who = 'fighter ' + $g.Name }
        Say ('  ' + $who + ' (' + $g.Count + ')')
        foreach ($o in ($g.Group | Sort-Object { [int]$_.costume_no })) {
            $src = [string]$o.source
            if ($o.kind -eq 'fluffy pak') { $src = 'Fluffy: ' + $src } else { $src = ($src -replace '/', '\') }
            Say ('    ' + $o.name.PadRight(14) + ' ' + $o.label + '   [' + $src + ']')
            $total++
        }
    }
    Say ('  Total: ' + $total + ' extra outfits (list of ' + $list.generated + ')')
    if ($list.left_out -and $list.left_out.Count -gt 0) {
        Say ''
        Say '  Left out, the game could not load them:'
        foreach ($l in $list.left_out) {
            $who = $names[[int]$l.fighter]
            Say ('    ' + $who + ': ' + $l.label + ' (' + $l.reason + ')')
        }
    }
}
Say ''
Say 'To report a problem: send this file and SF6_CostumeLoader.log (game folder) with the mod link.'

$report = Join-Path $PSScriptRoot 'SF6_CostumeAudit.txt'
[IO.File]::WriteAllLines($report, $out, (New-Object Text.UTF8Encoding $false))
$out | ForEach-Object { Write-Host $_ }
Write-Host ''
Write-Host ('Report saved to ' + $report)
