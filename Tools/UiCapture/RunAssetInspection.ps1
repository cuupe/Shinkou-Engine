param(
    [string]$BuildDirectory = 'out/build/mingw-debug',
    [string]$OutputDirectory = ('out/qa/asset-inspection-' + (Get-Date -Format 'yyyyMMdd-HHmmss')),
    [ValidateSet('dark','light','high-contrast')][string]$Theme = 'dark',
    [ValidateSet('full','narrow')][string]$Scenario = 'full'
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
$build = (Resolve-Path -LiteralPath (Join-Path $repo $BuildDirectory)).Path
$output = [IO.Path]::GetFullPath((Join-Path $repo $OutputDirectory))
if (-not $output.StartsWith($repo + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'QA output must stay inside the workspace' }
if (Test-Path -LiteralPath $output) { throw 'Use a new output directory; preserve existing evidence' }
Get-Command python,ffmpeg -ErrorAction Stop | Out-Null
$project = Join-Path $output 'project'
& python (Join-Path $PSScriptRoot 'CreateAssetInspectionFixtures.py') $project
if ($LASTEXITCODE -ne 0) { throw 'Asset fixture generation failed' }
$scriptFile = 'AssetInspection.txt'
if ($Scenario -eq 'narrow') {
    Copy-Item -LiteralPath (Join-Path $repo 'engine/tests/fixtures/ufbx-cube-7400-binary.fbx') -Destination (Join-Path $project 'assets/triangle.fbx')
    $scriptFile = 'AssetInspectionNarrow.txt'
}
& (Join-Path $build 'shinkou_ui_capture.exe') (Join-Path $build 'shinkou_engine_sample.exe') (Join-Path $output 'workspace.bmp') 5000 --client-size 1440x900 --require-gpu --queued-keyboard --script (Join-Path $PSScriptRoot $scriptFile) -- --editor dx11 --project $project --theme $Theme *> (Join-Path $output 'run.log')
$result = $LASTEXITCODE
Get-Content -LiteralPath (Join-Path $output 'run.log') -Tail 12
Write-Output ('Evidence: ' + $output)
if ($result -ne 0) { throw ('Native asset inspection failed with exit code ' + $result) }
