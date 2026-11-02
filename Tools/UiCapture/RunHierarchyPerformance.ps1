param(
    [string]$BuildDirectory = 'out/build/mingw-debug',
    [string]$OutputDirectory = ('out/qa/hierarchy-performance-' + (Get-Date -Format 'yyyyMMdd-HHmmss')),
    [ValidateSet('dx11','dx12','vulkan')][string]$Backend = 'dx11',
    [ValidateSet('dark','light','high-contrast')][string]$Theme = 'dark'
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
$build = (Resolve-Path -LiteralPath (Join-Path $repo $BuildDirectory)).Path
$output = [IO.Path]::GetFullPath((Join-Path $repo $OutputDirectory))
if (-not $output.StartsWith($repo + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'QA output must stay inside the workspace' }
if (Test-Path -LiteralPath $output) { throw 'Use a new evidence directory' }
$project = Join-Path $output 'project'
New-Item -ItemType Directory -Path (Join-Path $project 'assets') -Force | Out-Null
& (Join-Path $build 'shinkou_ui_capture.exe') (Join-Path $build 'shinkou_editor_ui_benchmarks.exe') (Join-Path $output 'workspace.bmp') 10000 --client-size 1440x900 --require-gpu --queued-keyboard --script (Join-Path $PSScriptRoot 'HierarchyPerformance.txt') -- --native $Backend $project 20000 $Theme *> (Join-Path $output 'run.log')
$result = $LASTEXITCODE
Get-Content -LiteralPath (Join-Path $output 'run.log') -Tail 10
Write-Output ('Evidence: ' + $output)
if ($result -ne 0) { throw ('Hierarchy native validation failed with exit code ' + $result) }
