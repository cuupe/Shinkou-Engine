param(
    [Parameter(Mandatory = $true)][string]$MetricsPath,
    [int]$MinimumSamples = 1,
    [string[]]$RequiredWorkloads = @('idle', 'hover', 'input', 'filter', 'scroll', 'resize')
)

$ErrorActionPreference = 'Stop'
$resolved = (Resolve-Path -LiteralPath $MetricsPath).Path
$rows = @(Import-Csv -LiteralPath $resolved)
if ($rows.Count -eq 0) { throw "No UI performance rows found: $resolved" }

$invalid = @($rows | Where-Object {
    $samples = 0
    -not [int]::TryParse([string]$_.samples, [ref]$samples) -or $samples -lt 0
})
if ($invalid.Count -gt 0) { throw "Invalid samples field in $($invalid.Count) UI performance rows: $resolved" }

$missing = @(
    foreach ($workload in $RequiredWorkloads) {
        $matches = @($rows | Where-Object {
            ([string]$_.workload).ToLowerInvariant() -eq $workload.ToLowerInvariant() -and
            ([int]$_.samples) -ge $MinimumSamples
        })
        if ($matches.Count -eq 0) { $workload }
    }
)
if ($missing.Count -gt 0) {
    throw "UI workload evidence is incomplete. Missing samples for: $($missing -join ', '). Metrics: $resolved"
}

Write-Output "UI performance metrics valid: $resolved"
foreach ($workload in $RequiredWorkloads) {
    $workloadRows = @($rows | Where-Object { ([string]$_.workload).ToLowerInvariant() -eq $workload.ToLowerInvariant() })
    $sampleTotal = ($workloadRows | ForEach-Object { [int]$_.samples } | Measure-Object -Sum).Sum
    Write-Output ("  {0}: rows={1} samples={2}" -f $workload, $workloadRows.Count, $sampleTotal)
}
