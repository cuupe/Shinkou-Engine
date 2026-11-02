param(
    [string]$BuildDirectory = 'out/build/mingw-debug',
    [string]$OutputDirectory = ('out/qa/viewport-' + (Get-Date -Format 'yyyyMMdd-HHmmss')),
    [ValidateSet('dark','light','high-contrast')][string]$Theme = 'dark',
    [ValidateSet('dx11','dx12','vulkan')][string]$Backend = 'dx11',
    [switch]$RenderValidation,
    [switch]$Detached,
    [switch]$Editing,
    [switch]$Performance
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
$scriptName = if ($Performance) { 'ViewportPerformance.txt' } elseif ($Editing) { 'ViewportEditing.txt' } elseif ($Detached) { 'DetachedViewportInteraction.txt' } else { 'ViewportInteraction.txt' }
$originalTemp=$env:TEMP; $originalTmp=$env:TMP
$shaderTemp=Join-Path $output 'shader-temp'
New-Item -ItemType Directory -Path $shaderTemp -Force | Out-Null
try {
    $env:TEMP=$shaderTemp; $env:TMP=$shaderTemp
    & (Join-Path $build 'shinkou_ui_capture.exe') (Join-Path $build 'shinkou_engine_sample.exe') (Join-Path $output 'interactive.bmp') 15000 --client-size 1440x900 --require-gpu --queued-keyboard --script (Join-Path $PSScriptRoot $scriptName) -- @engineArguments *> (Join-Path $output 'run.log')
    $result=$LASTEXITCODE
} finally { $env:TEMP=$originalTemp; $env:TMP=$originalTmp }
Get-Content -LiteralPath (Join-Path $output 'run.log') -Tail 12
Write-Output ('Evidence: ' + $output)
if ($result -ne 0) { throw ('Native viewport interaction failed with exit code ' + $result) }
if (Select-String -LiteralPath (Join-Path $output 'run.log') -Pattern 'Vulkan shader .+:|renderer-error=|graph-error=|Validation Error' -Quiet) { throw 'Native viewport emitted renderer/shader diagnostics; evidence is not accepted' }
