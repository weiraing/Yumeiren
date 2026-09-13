# Thin wrapper around the Phase4 sampler (tools/perf/sample2.ps1).
# Columns: timestamp,elapsed_s,ws_mb,private_mb,cpu_pct,handles,threads,gpu_pct,gpu_ded_mb,gpu_shared_mb
param(
    [string]$ProcName = "YumeirenTest",
    [int]$DurationSec = 60,
    [int]$IntervalSec = 5,
    [Parameter(Mandatory=$true)][string]$OutCsv
)
& (Join-Path $PSScriptRoot '..\..\tools\perf\sample2.ps1') -ProcName $ProcName `
    -DurationSec $DurationSec -IntervalSec $IntervalSec -OutCsv $OutCsv
