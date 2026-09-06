param(
    [string]$BuildDirectory = 'out/build/ui-current',
    [string]$OutputDirectory = ('out/qa/ui-interaction-' + (Get-Date -Format 'yyyyMMdd-HHmmss')),
    [ValidateSet('dark','light','high-contrast')][string]$Theme = 'dark'
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
$build = (Resolve-Path -LiteralPath (Join-Path $repo $BuildDirectory)).Path
$output = [IO.Path]::GetFullPath((Join-Path $repo $OutputDirectory))
if (-not $output.StartsWith($repo + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'QA output must stay inside the workspace' }
if (Test-Path -LiteralPath $output) { throw 'Use a new output directory; existing evidence is preserved' }
$project = Join-Path $output 'project'
New-Item -ItemType Directory -Path (Join-Path $project 'assets/Folder/Nested') -Force | Out-Null
[IO.File]::WriteAllText((Join-Path $project 'assets/Folder/Nested/needle.txt'),'fixture')
0..149 | ForEach-Object { [IO.File]::WriteAllText((Join-Path $project ('assets/item{0:D3}.txt' -f $_)), [string]$_) }
$script = (Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot 'EditorInteraction.txt')).Replace('@PROJECT@', $project.Replace('\','/'))
$scriptPath = Join-Path $output 'interaction.txt'
[IO.File]::WriteAllText($scriptPath,$script, [Text.UTF8Encoding]::new($false))
& (Join-Path $build 'shinkou_ui_capture.exe') (Join-Path $build 'shinkou_engine_sample.exe') (Join-Path $output 'interactive.bmp') 1500 --client-size 1920x1080 --require-gpu --script $scriptPath -- --editor dx11 --asset-view tree --project $project --theme $Theme *> (Join-Path $output 'run.log')
$result = $LASTEXITCODE
Get-Content -LiteralPath (Join-Path $output 'run.log') -Tail 13
Write-Output ('Evidence: ' + $output)
if ($result -ne 0) { throw ('Native interaction failed with exit code ' + $result) }
