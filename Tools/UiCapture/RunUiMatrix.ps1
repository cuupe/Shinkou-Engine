[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$CaptureExe,

    [Parameter(Mandatory = $true)]
    [string]$EditorExe,

    [string]$OutputDirectory = (Join-Path (Get-Location) "ui-capture-matrix"),

    [switch]$SkipGdiProbe,

    [switch]$FailOnCaptureFailure
)

$ErrorActionPreference = "Stop"

function Resolve-ExistingFile([string]$Path, [string]$Label) {
    $resolved = Resolve-Path -LiteralPath $Path -ErrorAction Stop
    if (-not (Test-Path -LiteralPath $resolved -PathType Leaf)) {
        throw "$Label is not a file: $resolved"
    }
    return $resolved.Path
}

function Read-BmpMetadata([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return [ordered]@{ valid = $false; width = 0; height = 0; bitCount = 0 }
    }
    try {
        $stream = [System.IO.File]::OpenRead($Path)
        $reader = [System.IO.BinaryReader]::new($stream)
        $magic = [char]$reader.ReadByte() + [char]$reader.ReadByte()
        $reader.ReadBytes(16) | Out-Null
        $width = $reader.ReadInt32()
        $rawHeight = $reader.ReadInt32()
        $reader.ReadBytes(2) | Out-Null
        $bitCount = $reader.ReadUInt16()
        $reader.Dispose()
        $stream.Dispose()
        return [ordered]@{
            valid = ($magic -eq "BM" -and $width -gt 0 -and $rawHeight -ne 0 -and $bitCount -eq 32)
            width = $width
            height = [Math]::Abs($rawHeight)
            bitCount = $bitCount
        }
    } catch {
        return [ordered]@{ valid = $false; width = 0; height = 0; bitCount = 0; error = $_.Exception.Message }
    }
}

function Get-Field([string]$Text, [string]$Pattern) {
    $match = [regex]::Match($Text, $Pattern, [System.Text.RegularExpressions.RegexOptions]::Multiline)
    if ($match.Success) { return $match.Groups[1].Value }
    return $null
}

function Get-Number([string]$Text, [string]$Pattern) {
    $value = Get-Field $Text $Pattern
    if ($null -eq $value) { return $null }
    return [UInt64]::Parse($value)
}

function Get-PassNames([string]$Text, [bool]$Present) {
    $names = [System.Collections.Generic.List[string]]::new()
    foreach ($line in ($Text -split "`r?`n")) {
        if ($line -notmatch '^pass ([^ ]+) cpu-ns=') { continue }
        $name = $Matches[1]
        $lower = $name.ToLowerInvariant()
        $isPresent = $lower.Contains("present")
        $isScene = $lower.Contains("scene") -or $lower.Contains("forward") -or $lower.Contains("world") -or $lower.Contains("mesh") -or $lower.Contains("sprite")
        if (($Present -and $isPresent) -or (-not $Present -and $isScene)) { $names.Add($name) }
    }
    return @($names)
}

function Get-RequestedBackend([string[]]$Arguments) {
    foreach ($argument in $Arguments) {
        switch ($argument.ToLowerInvariant()) {
            "dx11" { return "d3d11" }
            "d3d11" { return "d3d11" }
            "dx12" { return "d3d12" }
            "d3d12" { return "d3d12" }
            "vulkan" { return "vulkan" }
            "null" { return "null" }
        }
    }
    return "unspecified"
}

function New-ScenarioReport(
    [hashtable]$Scenario,
    [int]$ExitCode,
    [string]$RawOutput,
    [string]$ImagePath,
    [string]$ReportPath,
    [string]$RawLogPath
) {
    $surfaceMode = Get-Field $RawOutput 'mode=([^\s]+)'
    $surfaceKind = Get-Field $RawOutput 'surface-kind=([^\s]+)'
    $outer = Get-Field $RawOutput 'size=([0-9]+x[0-9]+)'
    $client = Get-Field $RawOutput 'client=([0-9]+x[0-9]+)'
    $surface = Get-Field $RawOutput 'surface=([0-9]+x[0-9]+)'
    $dpi = Get-Number $RawOutput '(?:^|\s)dpi=([0-9]+)(?:\s|$)'
    $actualDpiPercent = if ($null -ne $dpi) { [Math]::Round(($dpi * 100.0) / 96.0, 1) } else { $null }
    $requestedBackend = Get-RequestedBackend @($Scenario.Backend)
    $bmp = Read-BmpMetadata $ImagePath
    $commands = Get-Number $RawOutput 'editor-ui-commands=([0-9]+)'
    $textCommands = Get-Number $RawOutput '(?:editor-ui-text|ui-text-commands|text-commands)=([0-9]+)'
    $assetCount = Get-Number $RawOutput 'editor-ui-assets=([0-9]+)'
    $visibleAssetCount = Get-Number $RawOutput 'editor-ui-visible-assets=([0-9]+)'
    $engineTheme = Get-Field $RawOutput 'editor-ui-theme=([^\s]+)'
    $viewport = Get-Field $RawOutput 'editor-ui-viewport=([^\s]+)'
    $uiDpi = Get-Field $RawOutput 'editor-ui-dpi=([^\s]+)'
    $graphPasses = Get-Number $RawOutput 'render-graph passes=([0-9]+)'
    $executeNs = Get-Number $RawOutput 'execute-ns=([0-9]+)'
    $scenePasses = Get-PassNames $RawOutput $false
    $presentPasses = Get-PassNames $RawOutput $true
    # --client-size is a logical HWND client contract. GPU readback is in
    # physical pixels, so validate the HWND client first and report the GPU
    # surface separately for DPI evidence.
    $sizeMatches = $client -eq "$($Scenario.Width)x$($Scenario.Height)"
    $physicalSizeMatches = $false
    if ($bmp.valid -and $null -ne $dpi) {
        $scale = $dpi / 96.0
        $physicalSizeMatches = [Math]::Abs($bmp.width - [Math]::Round($Scenario.Width * $scale)) -le 2 -and
            [Math]::Abs($bmp.height - [Math]::Round($Scenario.Height * $scale)) -le 2
    }
    $dpiMatches = $null -ne $actualDpiPercent -and [Math]::Abs($actualDpiPercent - $Scenario.DpiPercent) -lt 0.1
    $captured = $ExitCode -eq 0 -and $bmp.valid
    $captureKindEvidence = $surfaceMode -eq "EngineGpuReadback" -or $surfaceMode -match '^WindowDC$|^BitBlt$|^PrintWindow$'
    $themeEvidencePresent = $Scenario.Theme -eq "unspecified" -or $null -ne $engineTheme
    $themeMatches = $Scenario.Theme -eq "unspecified" -or $engineTheme -eq $Scenario.Theme
    $qaStatus = if (-not $captured -or -not $sizeMatches -or
                   -not $themeEvidencePresent -or -not $themeMatches) { "fail" }
        elseif (-not $dpiMatches) { "inconclusive" }
        else { "pass" }

    return [ordered]@{
        schema = "shinkou.ui.capture-matrix.v1"
        scenario = $Scenario.Name
        captureStatus = if ($captured) { "captured" } else { "failed" }
        qaStatus = $qaStatus
        requested = [ordered]@{
            clientSize = "$($Scenario.Width)x$($Scenario.Height)"
            dpiPercent = $Scenario.DpiPercent
            theme = $Scenario.Theme
            backend = $requestedBackend
            forceGdi = [bool]$Scenario.ForceGdi
        }
        window = [ordered]@{
            outerSize = $outer
            clientSize = $client
        }
        surface = [ordered]@{
            kind = $surfaceKind
            captureMode = $surfaceMode
            width = $bmp.width
            height = $bmp.height
            bmpValid = $bmp.valid
            physicalSizeMatches = $physicalSizeMatches
            sourceContract = if ($surfaceMode -eq "EngineGpuReadback") { "SHINKOU_UI_CAPTURE_PATH" } else { "GDI fallback" }
        }
        backend = [ordered]@{
            requested = $requestedBackend
            reported = if ($RawOutput -match 'native-ui=([01])') { if ($Matches[1] -eq "1") { "d3d11" } else { "unsupported" } } else { $null }
            evidence = "child-log-capability"
        }
        dpi = [ordered]@{
            actual = $dpi
            actualPercent = $actualDpiPercent
            expectedPercent = $Scenario.DpiPercent
            matches = $dpiMatches
        }
        theme = [ordered]@{
            requested = $Scenario.Theme
            osEffective = $null
            engineReported = $engineTheme
            matches = $themeMatches
            evidence = if ($themeEvidencePresent) { "child-log" } else { "missing-from-child-log" }
        }
        ui = [ordered]@{
            commands = $commands
            text = $textCommands
            textEvidence = if ($null -ne $textCommands) { "child-log" } else { "missing-from-child-log" }
            assets = $assetCount
            visibleAssets = $visibleAssetCount
            assetEvidence = if ($null -ne $visibleAssetCount) { "child-log" } else { "missing-from-child-log" }
        }
        renderView = [ordered]@{
            rect = $viewport
            uiDpi = $uiDpi
            evidence = if ($null -ne $viewport) { "child-log" } else { "missing-from-child-log" }
        }
        renderGraph = [ordered]@{
            passCount = $graphPasses
            executeNs = $executeNs
            scenePasses = $scenePasses
            presentPasses = $presentPasses
            sceneEvidence = $scenePasses.Count -gt 0
            presentEvidence = $presentPasses.Count -gt 0
        }
        stageTimingNs = [ordered]@{
            modelSync = $null
            workspaceLayout = $null
            inputHitTest = $null
            paint = $null
            renderSubmit = $null
            uiBackend = $null
            gpuExecute = $executeNs
            evidence = "render-graph-child-log-partial"
        }
        checks = [ordered]@{
            surfaceIsGpuClient = $surfaceKind -eq "GpuClientSurface"
            surfaceIsGdiWindow = $surfaceKind -eq "GdiWindowSurface"
            captureModeRecognized = $captureKindEvidence
            sizeMatchesRequested = $sizeMatches
            physicalSizeMatchesExpectedDpi = $physicalSizeMatches
            dpiMatchesExpected = $dpiMatches
            commandsPresent = $null -ne $commands
            textPresent = $null -ne $textCommands
            assetsPresent = $null -ne $assetCount
            visibleAssetsPresent = $null -ne $visibleAssetCount
            scenePassPresent = $scenePasses.Count -gt 0
            presentPassPresent = $presentPasses.Count -gt 0
            themeEvidencePresent = $themeEvidencePresent
            themeMatches = $themeMatches
        }
        artifacts = [ordered]@{
            image = [System.IO.Path]::GetFullPath($ImagePath)
            rawLog = [System.IO.Path]::GetFullPath($RawLogPath)
            report = [System.IO.Path]::GetFullPath($ReportPath)
        }
        toolExitCode = $ExitCode
    }
}

$capturePath = Resolve-ExistingFile $CaptureExe "Capture executable"
$editorPath = Resolve-ExistingFile $EditorExe "Editor executable"
$outputRoot = [System.IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null

$scenarios = @(
    @{ Name = "dark-1280x720-100"; Width = 1280; Height = 720; DpiPercent = 100; Theme = "dark"; Backend = "dx11"; ForceGdi = $false },
    @{ Name = "dark-1600x900-125"; Width = 1600; Height = 900; DpiPercent = 125; Theme = "dark"; Backend = "dx11"; ForceGdi = $false },
    @{ Name = "dark-1024x768-100"; Width = 1024; Height = 768; DpiPercent = 100; Theme = "dark"; Backend = "dx11"; ForceGdi = $false },
    @{ Name = "light-1280x720-100"; Width = 1280; Height = 720; DpiPercent = 100; Theme = "light"; Backend = "dx11"; ForceGdi = $false },
    @{ Name = "high-contrast-1280x720-100"; Width = 1280; Height = 720; DpiPercent = 100; Theme = "high-contrast"; Backend = "dx11"; ForceGdi = $false }
)
if (-not $SkipGdiProbe) {
    $scenarios += @{ Name = "gdi-fallback-1280x720"; Width = 1280; Height = 720; DpiPercent = 100; Theme = "unspecified"; Backend = "dx11"; ForceGdi = $true }
}

$reports = [System.Collections.Generic.List[object]]::new()
foreach ($scenario in $scenarios) {
    $imagePath = Join-Path $outputRoot "$($scenario.Name).bmp"
    $reportPath = Join-Path $outputRoot "$($scenario.Name).report.json"
    $rawLogPath = Join-Path $outputRoot "$($scenario.Name).raw.log"
    $captureArguments = @(
        $editorPath, $imagePath, "5000",
        "--client-size", "$($scenario.Width)x$($scenario.Height)"
    )
    if ($scenario.ForceGdi) { $captureArguments += "--force-gdi" } else { $captureArguments += "--require-gpu" }
    $captureArguments += @("--", "--editor", $scenario.Backend)
    if ($scenario.Theme -ne "unspecified") { $captureArguments += @("--theme", $scenario.Theme) }

    Write-Host "[capture] $($scenario.Name)"
    $savedErrorActionPreference = $ErrorActionPreference
    try {
        # Windows PowerShell promotes native stderr to ErrorRecord. The
        # capture tool intentionally writes engine diagnostics to stderr, so
        # collect both streams as evidence without aborting the matrix.
        $ErrorActionPreference = "Continue"
        $nativeOutput = (& $capturePath @captureArguments 2>&1 | ForEach-Object { $_.ToString() } | Out-String)
    } finally {
        $ErrorActionPreference = $savedErrorActionPreference
    }
    [System.IO.File]::WriteAllText($rawLogPath, $nativeOutput)
    $exitCode = $LASTEXITCODE
    $report = New-ScenarioReport $scenario $exitCode $nativeOutput $imagePath $reportPath $rawLogPath
    $json = $report | ConvertTo-Json -Depth 12
    [System.IO.File]::WriteAllText($reportPath, $json)
    $reports.Add($report)
    Write-Host "  result=$($report.captureStatus) qa=$($report.qaStatus) exit=$exitCode"
}

$manifestPath = Join-Path $outputRoot "matrix.manifest.json"
$manifest = [ordered]@{
    schema = "shinkou.ui.capture-matrix-manifest.v1"
    generatedAtUtc = [DateTime]::UtcNow.ToString("o")
    captureExecutable = $capturePath
    editorExecutable = $editorPath
    scenarios = @($reports)
}
[System.IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 14))
Write-Host "[capture] manifest=$manifestPath"

if ($FailOnCaptureFailure -and @($reports | Where-Object { $_.captureStatus -ne "captured" }).Count -gt 0) {
    exit 5
}
exit 0
