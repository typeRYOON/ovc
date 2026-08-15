<#
.SYNOPSIS
  Build a ready-to-resolve ovc collab-merge fixture so you can click
  Merge... -> Open resolver and see the real conflict UI.

.DESCRIPTION
  Creates <Root>\ours and <Root>\theirs from an identical base .osu (+ audio),
  tracks both with ovc-cli, gives "theirs" diverging edits plus a NEW hitsample
  and exports <Root>\theirs.ovcz, then gives "ours" *conflicting* edits (left on
  disk, unsnapshotted -- the merge reads the live working file).

  The two histories share no git DAG; the merge pairs them by content hash
  (the shared base .osu blob), which is the whole point of the bundle model.

  Conflicts the resolver will show:
    field  OverallDifficulty   mine 9 / theirs 6   (was 8)
    note   0:01.000 col 0 hs   mine 2 / theirs 8   (was 0)
  Auto-merged (not shown): HPDrainRate 8->7, an added note, and the hitsample.

  Run with the desktop app CLOSED (both create and -Clean touch the shared
  registry). Start the desktop afterwards so it loads the new mapsets.

.PARAMETER Root
  Where to build the fixture folders + theirs.ovcz. Default: $HOME\ovc-merge-fixture

.PARAMETER Cli
  Path to ovc-cli.exe. Default: out\build\{release,debug}\ovc-cli.exe next to the repo.

.PARAMETER DataRoot
  ovc's data dir (registry + shadow repos). Default: %LOCALAPPDATA%\ovc

.PARAMETER Clean
  Remove everything this script created (shadow repos, registry entries, folders).

.EXAMPLE
  .\make-merge-fixture.ps1
  .\make-merge-fixture.ps1 -Clean
#>
[CmdletBinding()]
param(
  [string]$Root = "$HOME\ovc-merge-fixture",
  [string]$Cli,
  [string]$DataRoot = "$env:LOCALAPPDATA\ovc",
  [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$manifestPath = Join-Path $Root 'fixture-manifest.json'
# osu names a difficulty file "<Artist> - <Title> (<Creator>) [<Version>].osu";
# ours and theirs MUST share it so the merge pairs the two by path.
$diffName = 'ovc - Merge Test (you) [Resolver Demo].osu'

function Resolve-Cli([string]$Cli) {
  if ($Cli) {
    if (-not (Test-Path $Cli)) { throw "ovc-cli not found at: $Cli" }
    return (Resolve-Path $Cli).Path
  }
  foreach ($rel in 'release', 'debug') {
    $c = Join-Path $PSScriptRoot "..\out\build\$rel\ovc-cli.exe"
    if (Test-Path $c) { return (Resolve-Path $c).Path }
  }
  throw "ovc-cli.exe not found. Pass -Cli <path> (looked in out\build\{release,debug})."
}

# ---------------------------------------------------------------- clean --------
if ($Clean) {
  if (-not (Test-Path $manifestPath)) {
    Write-Host "No manifest at $manifestPath -- nothing to clean."
    return
  }
  $m = Get-Content $manifestPath -Raw | ConvertFrom-Json
  $drop = @($m.oursId, $m.theirsId) | Where-Object { $_ }

  foreach ($id in $drop) {
    $repo = Join-Path (Join-Path $DataRoot 'repos') $id
    if (Test-Path $repo) { Remove-Item $repo -Recurse -Force; Write-Host "removed repo $id" }
  }

  $index = Join-Path $DataRoot 'index.json'
  if (Test-Path $index) {
    Copy-Item $index "$index.bak" -Force
    $reg = Get-Content $index -Raw | ConvertFrom-Json
    $kept = @($reg.mapsets | Where-Object { $drop -notcontains $_.repoId })
    # Emit each entry compactly and re-wrap, so a 0/1-element result still
    # serializes as a JSON *array* (ConvertTo-Json would otherwise unwrap it).
    $inner = @($kept | ForEach-Object { $_ | ConvertTo-Json -Depth 12 -Compress })
    $json = '{"mapsets":[' + ($inner -join ',') + ']}'
    Set-Content $index $json -Encoding utf8
    Write-Host "registry cleaned ($($kept.Count) mapset(s) kept; backup at $index.bak)"
  }

  if (Test-Path $Root) { Remove-Item $Root -Recurse -Force }
  Write-Host "Done. Start the desktop app to confirm the test mapsets are gone."
  return
}

# --------------------------------------------------------------- create --------
$cliPath = Resolve-Cli $Cli
if (Test-Path $manifestPath) {
  throw "Fixture already exists at $Root. Run '.\make-merge-fixture.ps1 -Clean' first (desktop closed)."
}

$base = @'
osu file format v14

[General]
AudioFilename: audio.mp3
Mode: 3
SampleSet: Soft

[Metadata]
Title:Merge Test
TitleUnicode:Merge Test
Artist:ovc
ArtistUnicode:ovc
Creator:you
Version:Resolver Demo
BeatmapID:0
BeatmapSetID:-1

[Difficulty]
HPDrainRate:8
CircleSize:7
OverallDifficulty:8
ApproachRate:5
SliderMultiplier:1.4
SliderTickRate:1

[TimingPoints]
0,300,4,2,1,70,1,0

[HitObjects]
36,192,1000,1,0,0:0:0:0:
109,192,1250,1,0,0:0:0:0:
182,192,1500,1,2,0:0:0:0:
256,192,1750,1,0,0:0:0:0:
329,192,2000,128,0,3000:0:0:0:0:
'@

$noBom = New-Object System.Text.UTF8Encoding($false)
function Write-Text([string]$path, [string]$content) {
  [System.IO.File]::WriteAllText($path, $content, $noBom)
}

$oursDir  = Join-Path $Root 'ours'
$theirsDir = Join-Path $Root 'theirs'
New-Item -ItemType Directory -Force -Path $oursDir, $theirsDir | Out-Null

# Shared base audio -- identical bytes both sides, so it's not a media change.
$audio = New-Object byte[] 2000
Write-Text  (Join-Path $oursDir   $diffName) $base
Write-Text  (Join-Path $theirsDir $diffName) $base
[System.IO.File]::WriteAllBytes((Join-Path $oursDir   'audio.mp3'), $audio)
[System.IO.File]::WriteAllBytes((Join-Path $theirsDir 'audio.mp3'), $audio)

function Track([string]$folder) {
  $out = & $cliPath track $folder
  if ($LASTEXITCODE -ne 0) { throw "track failed for ${folder}:`n$($out -join "`n")" }
  $mm = ([regex]'repoId\s+(\S+)').Match(($out -join "`n"))
  if (-not $mm.Success) { throw "could not read repoId from:`n$($out -join "`n")" }
  return $mm.Groups[1].Value
}

Write-Host "Tracking ours + theirs (desktop should be CLOSED)..."
$oursId   = Track $oursDir
$theirsId = Track $theirsDir

# theirs: three overlapping edits (the conflicts) + one disjoint invisible one.
$theirs = $base.Replace('OverallDifficulty:8', 'OverallDifficulty:6')  # conflict: field
$theirs = $theirs.Replace('36,192,1000,1,0,', '36,192,1000,1,8,')      # conflict: note@1000 hitsound
$theirs = $theirs.Replace('128,0,3000:', '128,0,3500:')                # conflict: LN@2000 lengthened (shape)
$theirs = $theirs.Replace('HPDrainRate:8', 'HPDrainRate:7')            # disjoint HP change -> auto-merges (invisible)
Write-Text (Join-Path $theirsDir $diffName) $theirs
[System.IO.File]::WriteAllBytes((Join-Path $theirsDir 'soft-hitclap.wav'), [byte[]](@(1) * 400))

Write-Host "Snapshotting + exporting theirs.ovcz..."
& $cliPath snapshot $theirsDir
if ($LASTEXITCODE -ne 0) { throw "snapshot theirs failed" }
$bundle = Join-Path $Root 'theirs.ovcz'
& $cliPath export $theirsDir $bundle
if ($LASTEXITCODE -ne 0) { throw "export theirs failed" }

# ours: CONFLICTING edits, left on disk (the merge reads the live working file).
$ours = $base.Replace('OverallDifficulty:8', 'OverallDifficulty:9')
$ours = $ours.Replace('36,192,1000,1,0,', '36,192,1000,1,2,')      # note@1000 hitsound 0 -> 2
$ours = $ours.Replace('128,0,3000:', '128,0,2500:')               # LN@2000 shortened (shape)
Write-Text (Join-Path $oursDir $diffName) $ours

$manifest = [ordered]@{
  oursId = $oursId; theirsId = $theirsId
  oursFolder = $oursDir; theirsFolder = $theirsDir; bundle = $bundle; dataRoot = $DataRoot
}
($manifest | ConvertTo-Json -Depth 5) | Set-Content $manifestPath -Encoding utf8

Write-Host ""
Write-Host "Fixture ready." -ForegroundColor Green
Write-Host "  ours (merge INTO this) : $oursDir"
Write-Host "  ours repoId            : $oursId"
Write-Host "  theirs bundle          : $bundle"
Write-Host ""
Write-Host "Next (leave osu! closed so the preflight allows the write):"
Write-Host "  1. Start the desktop app -> two 'ovc - Merge Test' rows appear."
Write-Host "     Merge INTO the one from the '\ours' folder (repoId $oursId)."
Write-Host "     (Picking the other just reports 'nothing to merge' -- harmless.)"
Write-Host "  2. Merge... -> choose:  $bundle"
Write-Host "  3. It parks the merge (3 conflicts) -> Open resolver in the paired viewer."
Write-Host "  4. Pick sides -> apply. Then check $oursDir :"
Write-Host "       soft-hitclap.wav appears; OD + note hitsound + LN length match your picks."
Write-Host ""
Write-Host "Resolver shows (on the timeline: ours solid, theirs ghost, pick in the pane):"
Write-Host "  field  OverallDifficulty     mine 9 / theirs 6      (was 8)   -- pane only"
Write-Host "  note   0:01.000 col 0 hs     mine 2 / theirs 8      (was 0)   -- flagged note"
Write-Host "  note   0:02.000 shape        mine end 2500 / theirs 3500      -- LN ghost morphs"
Write-Host "  (theirs also lowers HP 8->7 -- a disjoint auto-merge, invisible on the timeline)"
Write-Host ""
Write-Host "Clean up (desktop CLOSED):  .\make-merge-fixture.ps1 -Clean"
