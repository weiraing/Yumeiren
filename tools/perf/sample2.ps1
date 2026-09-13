# Phase4 memory-attribution sampler (extends sample.ps1):
#   - gpu_ded_mb  = \GPU Process Memory(pid_*)\Local Usage   (GPU dedicated memory)
#   - gpu_shared_mb = \GPU Process Memory(pid_*)\Shared Usage (GPU shared/aperture memory)
#   - private_mb  = PrivateMemorySize64 = private committed bytes; for this process
#     this is effectively the Commit Size charge (shared-committed sections are
#     negligible). Do NOT confuse with Working Set: WS is resident RAM and is
#     trimmed by SetProcessWorkingSetSize without any real release.
param(
    [string]$ProcName = "YumeirenTest",
    [int]$DurationSec = 60,
    [int]$IntervalSec = 5,
    [Parameter(Mandatory=$true)][string]$OutCsv
)
$ErrorActionPreference = "Continue"
$cores = (Get-CimInstance Win32_ComputerSystem).NumberOfLogicalProcessors
$proc = Get-Process -Name $ProcName -ErrorAction Stop | Select-Object -First 1
$pid0 = $proc.Id
$startTime = $proc.StartTime
$prevCpu = $proc.TotalProcessorTime.TotalSeconds
$prevT = Get-Date
"timestamp,elapsed_s,ws_mb,private_mb,cpu_pct,handles,threads,gpu_pct,gpu_ded_mb,gpu_shared_mb" |
    Out-File -FilePath $OutCsv -Encoding utf8
$end = (Get-Date).AddSeconds($DurationSec)
while ((Get-Date) -lt $end) {
    Start-Sleep -Seconds $IntervalSec
    $now = Get-Date
    $p = Get-Process -Id $pid0 -ErrorAction SilentlyContinue
    if (-not $p) { break }
    $cpu = $p.TotalProcessorTime.TotalSeconds
    $wall = ($now - $prevT).TotalSeconds
    $cpuPct = [math]::Round(100 * ($cpu - $prevCpu) / $wall / $cores, 1)
    $prevCpu = $cpu; $prevT = $now
    $gpuPct = ""; $gpuDed = ""; $gpuShared = ""
    try {
        $c = Get-Counter -Counter "\GPU Engine(pid_$pid0*)\Utilization Percentage" -MaxSamples 2 -SampleInterval 1 -ErrorAction Stop
        $sets = @($c)
        $v = ($sets[-1].CounterSamples | Measure-Object CookedValue -Sum).Sum
        $gpuPct = [math]::Round([double]$v, 1)
    } catch {}
    try {
        $m = Get-Counter -Counter "\GPU Process Memory(pid_$pid0*)\Local Usage" -MaxSamples 1 -ErrorAction Stop
        $v = ($m.CounterSamples | Measure-Object CookedValue -Sum).Sum
        $gpuDed = [math]::Round([double]$v / 1MB, 1)
    } catch {}
    try {
        $m = Get-Counter -Counter "\GPU Process Memory(pid_$pid0*)\Shared Usage" -MaxSamples 1 -ErrorAction Stop
        $v = ($m.CounterSamples | Measure-Object CookedValue -Sum).Sum
        $gpuShared = [math]::Round([double]$v / 1MB, 1)
    } catch {}
    $elapsed = [math]::Round(($now - $startTime).TotalSeconds, 1)
    "$($now.ToString('HH:mm:ss')),$elapsed,$([math]::Round($p.WorkingSet64/1MB,1)),$([math]::Round($p.PrivateMemorySize64/1MB,1)),$cpuPct,$($p.HandleCount),$($p.Threads.Count),$gpuPct,$gpuDed,$gpuShared" |
        Out-File -FilePath $OutCsv -Append -Encoding utf8
}
