<#
  dist-win.ps1 — build and package a portable Windows (x64) release of ovc.

  Produces  dist/ovc-<version>-win-x64/       (the unpacked, runnable folder)
       and  dist/ovc-<version>-win-x64.zip    (what you upload to a GitHub release).

  The folder is self-contained: ovc.exe + ovc-cli.exe, the Qt runtime and plugins,
  and the MSVC runtime, all staged by windeployqt. No installer, no redist needed —
  a user unzips it and runs ovc.exe.

  Usage:
    powershell -ExecutionPolicy Bypass -File scripts\dist-win.ps1
    ...\dist-win.ps1 -SkipBuild                 # package an existing out/build/release
    ...\dist-win.ps1 -QtBin "C:\Qt\6.11.0\msvc2022_64\bin"
#>
[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [string]$QtBin = "C:\Qt\6.11.0\msvc2022_64\bin"
)
$ErrorActionPreference = 'Stop'

$root     = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $root 'out\build\release'

# --- version, straight from CMakeLists so the two never drift ---
$cml = Get-Content (Join-Path $root 'CMakeLists.txt') -Raw
if ($cml -match 'project\(ovc\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') { $version = $Matches[1] }
else { throw "couldn't read the version out of CMakeLists.txt" }
Write-Host "ovc $version - Windows x64 distribution" -ForegroundColor Cyan

# --- 1. build (Release-x64) unless told to reuse the existing tree ---
if (-not $SkipBuild) {
    Write-Host "== building (Release-x64)..."
    & (Join-Path $root 'build.bat') Release-x64
    if ($LASTEXITCODE -ne 0) { throw "build.bat failed (exit $LASTEXITCODE)" }
}
$exe = Join-Path $buildDir 'ovc.exe'
if (-not (Test-Path $exe)) { throw "ovc.exe not found at $exe - build first (drop -SkipBuild)" }

# --- 2. locate windeployqt ---
$windeployqt = Join-Path $QtBin 'windeployqt.exe'
if (-not (Test-Path $windeployqt)) {
    $cmd = Get-Command windeployqt -ErrorAction SilentlyContinue
    if ($cmd) { $windeployqt = $cmd.Source } else { $windeployqt = $null }
}
if (-not $windeployqt) { throw "windeployqt not found - pass -QtBin <Qt>\msvc2022_64\bin" }

# --- 3. clean staging dir ---
$stageName = "ovc-$version-win-x64"
$stage     = Join-Path $root "dist\$stageName"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null

# --- 4. copy the binaries in ---
Copy-Item $exe $stage
$cli = Join-Path $buildDir 'ovc-cli.exe'
if (Test-Path $cli) { Copy-Item $cli $stage } # ovc.exe's deploy is a superset of the CLI's deps

# --- 5. Qt runtime + plugins. --no-opengl-sw drops the ~20 MB software-GL fallback;
#        ovc is a plain Widgets app and never touches QtQuick/QOpenGLWidget. ---
Write-Host "== deploying Qt runtime..."
& $windeployqt --release --no-translations --no-opengl-sw (Join-Path $stage 'ovc.exe')
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed (exit $LASTEXITCODE)" }

# --- 6. MSVC runtime. windeployqt's --compiler-runtime needs a VS dev environment,
#        which this shell may not have; copying the redistributable DLLs out of
#        System32 is simpler and just as valid (they're the same files). ---
$sys = Join-Path $env:SystemRoot 'System32'
$rtDlls = 'vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll', 'msvcp140_1.dll',
          'msvcp140_2.dll', 'msvcp140_atomic_wait.dll', 'concrt140.dll'
$rtCopied = 0
foreach ($d in $rtDlls) {
    $src = Join-Path $sys $d
    if (Test-Path $src) { Copy-Item $src $stage; $rtCopied++ }
}
if ($rtCopied -lt 3) {
    Write-Warning "only $rtCopied MSVC runtime DLL(s) bundled - users may need the 'VC++ 2015-2022 x64' redistributable"
} else {
    Write-Host "== bundled $rtCopied MSVC runtime DLL(s)"
}

# --- 7. docs alongside the binaries ---
foreach ($f in 'README.md', 'LICENSE') {
    $p = Join-Path $root $f
    if (Test-Path $p) { Copy-Item $p $stage }
}

# --- 8. zip it ---
$zip = Join-Path $root "dist\$stageName.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
$size = "{0:N1} MB" -f ((Get-Item $zip).Length / 1MB)
Write-Host "== packaged: $zip ($size)" -ForegroundColor Green
Write-Host "   staged at: $stage"
