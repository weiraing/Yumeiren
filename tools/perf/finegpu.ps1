# Fine-grained GPU utilization sampling to find render-start latency.
param(
    [Parameter(Mandatory=$true)][int]$ProcId,
    [int]$DurationSec = 15,
    [double]$IntervalMs = 250
)
$sw = [Diagnostics.Stopwatch]::StartNew()
while ($sw.Elapsed.TotalSeconds -lt $DurationSec) {
    $t = $sw.Elapsed.TotalMilliseconds
    $gpu = ""
    try {
        $c = Get-Counter -Counter "\GPU Engine(pid_$ProcId*)\Utilization Percentage" -MaxSamples 2 -SampleInterval 1 -ErrorAction Stop
        $sets = @($c)
        $v = ($sets[-1].CounterSamples | Measure-Object CookedValue -Sum).Sum
        $gpu = [math]::Round([double]$v, 1)
    } catch { $gpu = "?" }
    Write-Host ("{0,8:N0}ms gpu={1}" -f $t, $gpu)
    Start-Sleep -Milliseconds $IntervalMs
}
