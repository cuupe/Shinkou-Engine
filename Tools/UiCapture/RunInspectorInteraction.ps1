param(
    [string]$BuildDirectory = 'out/build/mingw-debug',
    [string]$OutputDirectory = ('out/qa/inspector-' + (Get-Date -Format 'yyyyMMdd-HHmmss')),
    [ValidateSet('dark','light','high-contrast')][string]$Theme = 'dark',
    [ValidateSet('dx11','dx12','vulkan')][string]$Backend = 'dx11',
    [switch]$RenderValidation
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
$build = (Resolve-Path -LiteralPath (Join-Path $repo $BuildDirectory)).Path
$output = [IO.Path]::GetFullPath((Join-Path $repo $OutputDirectory))
if (-not $output.StartsWith($repo + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'QA output must stay inside the workspace' }
if (Test-Path -LiteralPath $output) { throw 'Use a new output directory; existing evidence is preserved' }
$project = Join-Path $output 'project'
New-Item -ItemType Directory -Path (Join-Path $project 'assets') -Force | Out-Null
$engineArguments = @('--editor', $Backend, '--project', $project, '--theme', $Theme)
if ($RenderValidation) { $engineArguments += '--render-validation' }
& (Join-Path $build 'shinkou_ui_capture.exe') (Join-Path $build 'shinkou_engine_sample.exe') (Join-Path $output 'interactive.bmp') 15000 --client-size 1440x900 --require-gpu --queued-keyboard --script (Join-Path $PSScriptRoot 'InspectorInteraction.txt') -- @engineArguments *> (Join-Path $output 'run.log')
$result = $LASTEXITCODE
Get-Content -LiteralPath (Join-Path $output 'run.log') -Tail 12
Write-Output ('Evidence: ' + $output)
if ($result -ne 0) { throw ('Native inspector interaction failed with exit code ' + $result) }
