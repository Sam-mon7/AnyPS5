[CmdletBinding(DefaultParameterSetName = 'Gate')]
param(
    [string[]]$Experiments,
    [Parameter(Mandatory, ParameterSetName = 'Gate')][string]$Control,
    [string]$Candidate,
    [string]$Matrix,
    [Parameter(Mandatory, ParameterSetName = 'Gate')][string]$Output,
    [string]$Runtime = (Join-Path $HOME 'AnyPS5-DS-replay'),
    [string]$NavigationRefs = (Join-Path $HOME 'AnyPS5-redesign\automation\navref'),
    [ValidateSet('Bootstrap', 'Child', 'Milestone', 'Final')][string]$Mode = 'Child',
    [ValidateRange(0, 3600)][int]$WarmupSeconds = 30,
    [ValidateRange(1, 3600)][int]$HoldSeconds = 120,
    [switch]$Alternating,
    [ValidateRange(1, 3600)][int]$BlockSeconds = 15,
    [ValidateRange(0, 3599)][int]$SettleSeconds = 3,
    [ValidateRange(1, 1000)][int]$MinFreeGB = 10,
    [ValidateRange(0, 1048576)][int]$MinAvailableMiB = 256,
    [ValidateRange(1, 3600)][int]$LowMemorySeconds = 60,
    [switch]$Bisect,
    [Parameter(Mandatory, ParameterSetName = 'Watchdog')][string]$WatchdogManifest,
    [Parameter(Mandatory, ParameterSetName = 'Watchdog')][int]$RunnerPid,
    [Parameter(Mandatory, ParameterSetName = 'Watchdog')][long]$RunnerStartTicks
)

$ErrorActionPreference = 'Stop'
$script:GatePython = (Get-Command python -ErrorAction Stop).Source
$script:GateTools = $PSScriptRoot
$script:GateScript = $PSCommandPath
$script:AllowAlternating = -not $PSBoundParameters.ContainsKey('Alternating') -or [bool]$Alternating

function Move-GateFile([string]$Source, [string]$Destination) {
    for ($attempt = 0; ; ++$attempt) {
        try { [IO.File]::Move($Source, $Destination, $true); return }
        catch [IO.IOException], [UnauthorizedAccessException] {
            if ($attempt -ge 100) { throw }
            Start-Sleep -Milliseconds 10
        }
    }
}

function Write-GateJson([string]$Path, $Value) {
    $temporary = $Path + '.' + [Guid]::NewGuid().ToString('N')
    try {
        [IO.File]::WriteAllText($temporary, (ConvertTo-Json -InputObject $Value -Depth 80), [Text.UTF8Encoding]::new($false))
        Move-GateFile $temporary $Path
    } finally { if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary } }
}

function Invoke-GatePython([string]$Tool, [string[]]$Arguments, [int[]]$AllowedCodes = @(0)) {
    $result = & $script:GatePython (Join-Path $script:GateTools $Tool) @Arguments
    $code = $LASTEXITCODE
    if ($code -notin $AllowedCodes) { Stop-GateFailure $code "$Tool exited with code $code" }
    if ($result) { return ($result -join "`n" | ConvertFrom-Json -AsHashtable) }
}

function Stop-GateFailure([int]$Code, [string]$Message) {
    $failure = [Exception]::new($Message)
    $failure.Data['GateCode'] = $Code
    throw $failure
}

function Test-GateProcess([int]$ProcessId, [long]$StartTicks) {
    $process = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
    return $null -ne $process -and $process.StartTime.ToUniversalTime().Ticks -eq $StartTicks
}

function Stop-GateProcess([int]$ProcessId, [long]$StartTicks) {
    $process = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
    if ($null -eq $process) { return }
    if ($process.StartTime.ToUniversalTime().Ticks -ne $StartTicks) { Stop-GateFailure 30 'Refusing to stop a PID with a different creation time' }
    $process | Stop-Process -Force
    $process.WaitForExit()
}

function Restore-GateRuntime([string]$Manifest) {
    return Invoke-GatePython 'archb_gate_config.py' @('restore', $Manifest)
}

function Start-GateWatchdog([string]$Manifest, [int]$ProcessId, [long]$StartTicks) {
    $arguments = @('-NoProfile', '-File', "`"$script:GateScript`"", '-WatchdogManifest', "`"$Manifest`"",
                   '-RunnerPid', $ProcessId, '-RunnerStartTicks', $StartTicks)
    $commandLine = "`"$((Get-Command pwsh).Source)`" -WindowStyle Hidden " + ($arguments -join ' ')
    $created = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{ CommandLine = $commandLine }
    if ($created.ReturnValue -ne 0) { Stop-GateFailure 31 "Watchdog could not start outside the runner's process tree ($($created.ReturnValue))" }
    return Get-Process -Id $created.ProcessId
}

function Invoke-GateWatchdog([string]$Manifest, [int]$ProcessId, [long]$StartTicks) {
    while (Test-GateProcess $ProcessId $StartTicks) {
        Start-Sleep -Milliseconds 250
    }
    $mutex = [Threading.Mutex]::new($false, 'Global\AnyPS5GateRuntime')
    $owned = $false
    try {
        try { $owned = $mutex.WaitOne(30000) } catch [Threading.AbandonedMutexException] { $owned = $true }
        if (-not $owned) { Stop-GateFailure 31 'Watchdog could not acquire runtime mutex' }
        $plan = Get-Content -LiteralPath $Manifest -Raw | ConvertFrom-Json -AsHashtable
        if ($plan.status -ne 'pending') { return }
        $activePath = Join-Path (Split-Path $Manifest -Parent) 'active-processes.json'
        $active = @{}
        if (Test-Path -LiteralPath $activePath) {
            $active = Get-Content -LiteralPath $activePath -Raw | ConvertFrom-Json -AsHashtable
            foreach ($key in @('navigator', 'game')) {
                if ($active.ContainsKey($key) -and (Test-GateProcess $active[$key].id $active[$key].startTicks)) {
                    Stop-GateProcess $active[$key].id $active[$key].startTicks
                }
            }
        }
        $restored = Restore-GateRuntime $Manifest
        $gatePath = Join-Path (Split-Path $Manifest -Parent) 'gate.json'
        $gate = if (Test-Path -LiteralPath $gatePath) { Get-Content -LiteralPath $gatePath -Raw | ConvertFrom-Json -AsHashtable } else { @{} }
        $gate.restoration = $restored
        $gate.code = 11
        $gate.verdict = 'runner terminated; watchdog restored runtime'
        $artifactRoot = Split-Path $Manifest -Parent
        $gate.legs = @(Get-ChildItem -LiteralPath $artifactRoot -Filter 'leg.json' -Recurse -File | ForEach-Object {
            Get-Content -LiteralPath $_.FullName -Raw | ConvertFrom-Json -AsHashtable
        })
        $runManifest = Join-Path $artifactRoot 'manifest.json'
        if (Test-Path -LiteralPath $runManifest) {
            $run = Get-Content -LiteralPath $runManifest -Raw | ConvertFrom-Json -AsHashtable
            $gate.wall_clock_seconds = ([DateTime]::UtcNow - [DateTime]::Parse($run.started_utc).ToUniversalTime()).TotalSeconds
        }
        try { $null = Invoke-GatePython 'archb_gate_nav.py' @('contact', $artifactRoot) }
        catch { $gate.contact_error = $_.Exception.Message }
        Write-GateJson $gatePath $gate
    } finally {
        if ($owned) { $mutex.ReleaseMutex() }
        $mutex.Dispose()
    }
}

function Initialize-GateMemory {
    if ('ArchBGateNative' -as [type]) { return }
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class ArchBGateNative {
    [StructLayout(LayoutKind.Sequential)]
    public struct ProcessMemory {
        public uint Size;
        public uint PageFaults;
        public UIntPtr PeakWorkingSet;
        public UIntPtr WorkingSet;
        public UIntPtr PeakPagedPool;
        public UIntPtr PagedPool;
        public UIntPtr PeakNonPagedPool;
        public UIntPtr NonPagedPool;
        public UIntPtr Pagefile;
        public UIntPtr PeakPagefile;
        public UIntPtr PrivateUsage;
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct SystemMemory {
        public uint Size;
        public uint Load;
        public ulong TotalPhysical;
        public ulong AvailablePhysical;
        public ulong TotalPagefile;
        public ulong AvailablePagefile;
        public ulong TotalVirtual;
        public ulong AvailableVirtual;
        public ulong AvailableExtendedVirtual;
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct PerformanceInfo {
        public uint Size;
        public UIntPtr CommitTotal;
        public UIntPtr CommitLimit;
        public UIntPtr CommitPeak;
        public UIntPtr PhysicalTotal;
        public UIntPtr PhysicalAvailable;
        public UIntPtr SystemCache;
        public UIntPtr KernelTotal;
        public UIntPtr KernelPaged;
        public UIntPtr KernelNonPaged;
        public UIntPtr PageSize;
        public uint HandleCount;
        public uint ProcessCount;
        public uint ThreadCount;
    }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct DisplayMode {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string Device;
        public ushort SpecVersion;
        public ushort DriverVersion;
        public ushort Size;
        public ushort Extra;
        public uint Fields;
        public int PositionX;
        public int PositionY;
        public uint Orientation;
        public uint FixedOutput;
        public short Color;
        public short Duplex;
        public short YResolution;
        public short TTOption;
        public short Collate;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string Form;
        public ushort LogPixels;
        public uint BitsPerPixel;
        public uint Width;
        public uint Height;
        public uint Flags;
        public uint Frequency;
        public uint IcmMethod;
        public uint IcmIntent;
        public uint MediaType;
        public uint DitherType;
        public uint Reserved1;
        public uint Reserved2;
        public uint PanningWidth;
        public uint PanningHeight;
    }
    [DllImport("psapi.dll", SetLastError = true)]
    public static extern bool GetProcessMemoryInfo(IntPtr process, ref ProcessMemory memory, uint size);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool GlobalMemoryStatusEx(ref SystemMemory memory);
    [DllImport("psapi.dll", SetLastError = true)]
    public static extern bool GetPerformanceInfo(ref PerformanceInfo information, uint size);
    [DllImport("user32.dll")]
    public static extern IntPtr GetDC(IntPtr window);
    [DllImport("user32.dll")]
    public static extern int ReleaseDC(IntPtr window, IntPtr dc);
    [DllImport("gdi32.dll")]
    public static extern int GetDeviceCaps(IntPtr dc, int capability);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern bool EnumDisplaySettings(string device, int mode, ref DisplayMode display);
}
"@
}

function Get-GateMemory($Process) {
    $memory = [ArchBGateNative+ProcessMemory]::new()
    $memory.Size = [Runtime.InteropServices.Marshal]::SizeOf($memory)
    $system = [ArchBGateNative+SystemMemory]::new()
    $system.Size = [Runtime.InteropServices.Marshal]::SizeOf($system)
    $performance = [ArchBGateNative+PerformanceInfo]::new()
    $performance.Size = [Runtime.InteropServices.Marshal]::SizeOf($performance)
    if (-not [ArchBGateNative]::GetPerformanceInfo([ref]$performance, $performance.Size) -or
        -not [ArchBGateNative]::GetProcessMemoryInfo($Process.Handle, [ref]$memory, $memory.Size) -or
        -not [ArchBGateNative]::GlobalMemoryStatusEx([ref]$system)) { Stop-GateFailure 26 'Cannot read process or system memory' }
    return @{ private_commit = $memory.PrivateUsage.ToUInt64(); working_set = $memory.WorkingSet.ToUInt64()
              peak_private_commit = $memory.PeakPagefile.ToUInt64(); peak_working_set = $memory.PeakWorkingSet.ToUInt64()
              available_memory = $system.AvailablePhysical; commit_headroom = ($performance.CommitLimit.ToUInt64() - $performance.CommitTotal.ToUInt64()) * $performance.PageSize.ToUInt64() }
}

function Get-GateDisplay {
    $display = [ArchBGateNative+DisplayMode]::new()
    $display.Size = [Runtime.InteropServices.Marshal]::SizeOf($display)
    if ([ArchBGateNative]::EnumDisplaySettings($null, -1, [ref]$display)) {
        return @{ width = $display.Width; height = $display.Height; refresh_hz = $display.Frequency; source = 'EnumDisplaySettings' }
    }
    $dc = [ArchBGateNative]::GetDC([IntPtr]::Zero)
    if ($dc -eq [IntPtr]::Zero) { Stop-GateFailure 30 'Cannot read current display mode' }
    try {
        $result = @{ width = [ArchBGateNative]::GetDeviceCaps($dc, 118); height = [ArchBGateNative]::GetDeviceCaps($dc, 117)
                     refresh_hz = [ArchBGateNative]::GetDeviceCaps($dc, 116); source = 'GetDeviceCaps' }
        if ($result.width -le 0 -or $result.height -le 0 -or $result.refresh_hz -le 1) { Stop-GateFailure 30 'Display resolution or refresh rate is unavailable' }
        return $result
    } finally { $null = [ArchBGateNative]::ReleaseDC([IntPtr]::Zero, $dc) }
}

function Get-GateClock {
    return [long]([Diagnostics.Stopwatch]::GetTimestamp() * (1000000000.0 / [Diagnostics.Stopwatch]::Frequency))
}

function Add-GateMemorySample($Process, [string]$Directory, [ref]$LowSince) {
    if ($Process.HasExited) { Stop-GateFailure 24 'Game exited before leg completed' }
    $sample = Get-GateMemory $Process
    $timestamp = Get-GateClock
    "$timestamp,$($sample.private_commit),$($sample.working_set),$($sample.commit_headroom),$($sample.available_memory)" |
        Add-Content -LiteralPath (Join-Path $Directory 'memory.csv') -Encoding utf8
    foreach ($record in @($script:ActiveLeg, $script:ActiveBlock)) {
        if ($null -eq $record) { continue }
        if ($null -eq $record.min_available_memory -or $sample.available_memory -lt $record.min_available_memory) {
            $record.min_available_memory = $sample.available_memory
        }
        if ($record.measured) { $record.memory_pressure = $record.min_available_memory -lt 512MB }
    }
    if ($sample.available_memory -lt ([long]$MinAvailableMiB * 1MB)) {
        if ($null -eq $LowSince.Value) { $LowSince.Value = $timestamp }
        if ($timestamp - $LowSince.Value -ge ([long]$LowMemorySeconds * 1000000000)) { Stop-GateFailure 26 "Available memory below $MinAvailableMiB MiB for $LowMemorySeconds seconds" }
    } else { $LowSince.Value = $null }
    if ($sample.commit_headroom -lt 4GB) { Stop-GateFailure 26 'System commit headroom below 4 GiB' }
    return $sample
}

function Wait-GateHold($Process, [string]$Directory, [int]$Seconds, [ref]$LowSince) {
    $until = (Get-GateClock) + [long]$Seconds * 1000000000
    while ((Get-GateClock) -lt $until) {
        $null = Add-GateMemorySample $Process $Directory $LowSince
        Start-Sleep -Milliseconds 1000
    }
}

function Deploy-GateConfiguration($Configuration) {
    foreach ($name in $Configuration.libs.Keys) {
        $source = $Configuration.sources[$name]
        foreach ($kind in @('patched', 'unpatched')) {
            $destination = if ($kind -eq 'patched') { Join-Path $Runtime "libs/$name" } else { Join-Path $Runtime "libs/unpatched/$name" }
            $expected = if ($kind -eq 'patched') { $Configuration.libs[$name] } else { $source.unpatched_sha256 }
            if ((Get-FileHash -LiteralPath $source[$kind] -Algorithm SHA256).Hash -ne $expected) { Stop-GateFailure 23 "Source library changed: $name" }
            Copy-Item -LiteralPath $source[$kind] -Destination $destination -Force
            if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ne $expected) { Stop-GateFailure 23 "Deployment hash mismatch: $destination" }
        }
    }
}

function Write-GateControl([string]$Directory, $Values) {
    $remainder = 0L
    $sequence = [Math]::DivRem(([DateTime]::UtcNow.Ticks - [DateTime]::UnixEpoch.Ticks), 10L, [ref]$remainder)
    $tokens = @($Values.Keys | Sort-Object | ForEach-Object { "$_=$($Values[$_])" })
    $path = Join-Path $Directory 'control.txt'
    $temporary = $path + '.tmp'
    try {
        [IO.File]::WriteAllText($temporary, "$sequence $($tokens -join ' ')`n", [Text.Encoding]::ASCII)
        Move-GateFile $temporary $path
    } finally { if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary } }
    return $sequence
}

function Wait-GateToggle($Process, [string]$Directory, [long]$Sequence, $LowSince = $null) {
    $deadline = (Get-GateClock) + 10000000000
    $nextSample = (Get-GateClock) + 1000000000
    while ((Get-GateClock) -lt $deadline) {
        if ($Process.HasExited) { Stop-GateFailure 24 'Game exited while awaiting toggle' }
        $text = [IO.File]::ReadAllText((Join-Path $Directory 'stderr.log'))
        if ($text -match '(?m)^\[archb\] toggle refused seq=\d+[^\r\n]*') { Stop-GateFailure 21 $Matches[0] }
        if ($text -match "(?m)^\[archb\] toggle seq=$Sequence t_ns=(\d+) [^\r\n]+`r?`$") { return [long]$Matches[1] }
        if ($null -ne $LowSince -and (Get-GateClock) -ge $nextSample) {
            $null = Add-GateMemorySample $Process $Directory $LowSince
            $nextSample = (Get-GateClock) + 1000000000
        }
        Start-Sleep -Milliseconds 50
    }
    Stop-GateFailure 24 "No acknowledgement for toggle $Sequence within 10s"
}

function Invoke-GateBlocks($Process, [string]$Directory, $Plan, [ref]$LowSince) {
    $blocks = [Collections.Generic.List[object]]::new()
    $script:ActiveBlocks = $blocks
    $stages = if ($Mode -eq 'Bootstrap') { @(48) } else { @(24, 48, 96) }
    foreach ($total in $stages) {
        while ($blocks.Count -lt $total) {
            $order = if ([Security.Cryptography.RandomNumberGenerator]::GetInt32(2)) { @('control', 'candidate') } else { @('candidate', 'control') }
            foreach ($role in $order) {
                $sequence = Write-GateControl $Directory $Plan[$role]
                $applied = Wait-GateToggle $Process $Directory $sequence $LowSince
                if ($null -ne $script:ActiveBlock) { $script:ActiveBlock.end_seq = $sequence }
                $block = @{ role = $role; pair = [int][Math]::Floor($blocks.Count / 2); start_seq = $sequence
                            values = $Plan[$role]; applied_ns = $applied; measured = $true; min_available_memory = $null; memory_pressure = $false }
                $script:ActiveBlock = $block
                $blocks.Add($block)
                $null = Add-GateMemorySample $Process $Directory $LowSince
                Write-GateJson (Join-Path $Directory 'blocks.json') @($blocks.ToArray())
                $deadline = $applied + [long]$BlockSeconds * 1000000000
                while ((Get-GateClock) -lt $deadline) {
                    $null = Add-GateMemorySample $Process $Directory $LowSince
                    Start-Sleep -Milliseconds 1000
                }
                if ($blocks.Count -ge $total - 1) {
                    $null = Invoke-GatePython 'archb_gate_nav.py' @('screenshot', '--pid', $Process.Id, '--output', (Join-Path $Directory "last-$role.png"))
                }
            }
        }
        $sequence = Write-GateControl $Directory $Plan.control
        $null = Wait-GateToggle $Process $Directory $sequence $LowSince
        $script:ActiveBlock.end_seq = $sequence
        $script:ActiveBlock = $null
        Write-GateJson (Join-Path $Directory 'blocks.json') @($blocks.ToArray())
        Start-Sleep -Milliseconds 250
        $arguments = @('alternating', $Directory, '--settle-seconds', $SettleSeconds, '--output', (Join-Path $Directory 'alternating.json'))
        if ($Plan.ContainsKey('experiment')) {
            $arguments = @('ub-experiment', $Directory, '--name', $Plan.experiment, '--settle-seconds', $SettleSeconds,
                           '--output', (Join-Path $Directory 'ub-experiment.json'))
            foreach ($name in $Plan.switches) { $arguments += @('--switch', $name) }
        }
        elseif ($Mode -eq 'Bootstrap') { $arguments += '--bootstrap' }
        else { $arguments += @('--baseline', $script:BaselinePath) }
        $decision = Invoke-GatePython 'archb_gate_stats.py' $arguments @(0, 1, 2, 3, 27, 40)
        if ($decision.verdict -ne 'continue') { return $decision }
    }
}

function Get-GateAlternatingPlan($ControlLeg, $CandidateLeg) {
    $arguments = @('alternating-plan', (Join-Path $ControlLeg.directory 'configuration.json'),
                   (Join-Path $CandidateLeg.directory 'configuration.json'), (Join-Path $ControlLeg.directory 'stderr.log'),
                   (Join-Path $CandidateLeg.directory 'stderr.log'))
    if ($Mode -eq 'Bootstrap') { $arguments += '--bootstrap' }
    return Invoke-GatePython 'archb_gate_config.py' $arguments
}

function Invoke-GateUpperBoundExperiments($Configuration) {
    $names = @('UB_TRUST_PROOFS', 'UB_NO_LOCK_INVALIDATION', 'UB_POINTER_FREE_DRAW_KEYS', 'UB_SKIP_CPU_INDIRECT', 'UB_SKIP_DISPATCH_PROOF_WALK')
    $requested = @($Experiments | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Select-Object -Unique)
    if (@($requested | Where-Object { $_ -notin $names }).Count) { Stop-GateFailure 21 'Unknown upper-bound experiment' }
    $results = @()
    foreach ($experiment in @($requested) + @('ALL_UB')) {
        $enabled = if ($experiment -eq 'ALL_UB') { $names } else { @($experiment) }
        $off = $Configuration | ConvertTo-Json -Depth 80 | ConvertFrom-Json -AsHashtable
        $off.name = 'ub-' + $experiment.ToLowerInvariant()
        $off.requiredCounters = @()
        $plan = @{ control = @{}; candidate = @{}; experiment = $experiment; switches = @($enabled) }
        foreach ($name in $names) {
            $off.env["APS5_ARCHB_$name"] = '0'
            $off.expectSwitches[$name] = '0'
            $plan.control[$name] = '0'
            $plan.candidate[$name] = if ($name -in $enabled) { '1' } else { '0' }
        }
        try {
            $leg = Invoke-GateLeg $off $off.name 'measured' 0 $false $plan
            $results += $leg.alternating
        } catch {
            $failed = $script:AllLegs[$script:AllLegs.Count - 1]
            $hits = @{}
            $stderr = Join-Path $failed.directory 'stderr.log'
            if (Test-Path -LiteralPath $stderr) {
                $text = [IO.File]::ReadAllText($stderr)
                foreach ($name in $enabled) {
                    $counter = $name.ToLowerInvariant() + '_hits'
                    $values = @([regex]::Matches($text, "\b$counter=([0-9]+)\b") | ForEach-Object { [long]$_.Groups[1].Value })
                    $hits[$counter] = if ($values.Count) { ($values | Measure-Object -Maximum).Maximum } else { 0 }
                }
            }
            $results += @{ name = $experiment; switches = @($enabled); status = 'experiment crashed'; stage = $failed.stage
                           error = $_.Exception.Message; directory = $failed.directory; hits = $hits }
        }
        $path = Join-Path $Output 'ub-experiments.json'
        Write-GateJson $path @($results)
        $report = Invoke-GatePython 'archb_gate_stats.py' @('ub-report', $path, '--output', (Join-Path $Output 'ub-report.json'), '--table', (Join-Path $Output 'ub-report.md'))
    }
    Write-Host $report.markdown
    return @($results)
}

function Invoke-GateBisection($Configuration, $Plan) {
    $enabled = @($Plan.metadata.Keys | Where-Object { $_ -ne 'GATE_RUN' -and [long]$Plan.metadata[$_].value -ne 0 })
    if (@($enabled | Where-Object { -not $Plan.metadata[$_].runtime }).Count) { Stop-GateFailure 21 'Bisection requires every enabled feature to be runtime' }
    if (-not $enabled.Count) { Stop-GateFailure 21 'Bisection needs enabled runtime switches' }
    $attempts = @()
    $iteration = 0
    while ($true) {
        $groups = if ($enabled.Count -eq 1) { ,$enabled } else {
            $middle = [int][Math]::Ceiling($enabled.Count / 2)
            ,@($enabled[0..($middle - 1)])
            ,@($enabled[$middle..($enabled.Count - 1)])
        }
        $reproduced = $false
        foreach ($group in $groups) {
            $trial = @{ control = @{}; candidate = @{} }
            foreach ($name in $Plan.control.Keys) {
                $trial.control[$name] = '0'
                $trial.candidate[$name] = if ($name -in $group) { $Plan.candidate[$name] } else { '0' }
            }
            $off = $Configuration | ConvertTo-Json -Depth 80 | ConvertFrom-Json -AsHashtable
            foreach ($name in $trial.control.Keys) {
                $off.env["APS5_ARCHB_$name"] = '0'
                $off.expectSwitches[$name] = '0'
            }
            ++$iteration
            $leg = Invoke-GateLeg $off "$($Configuration.name)-bisect-$iteration" 'measured' 0 $true $trial
            $attempts += @{ switches = @($group); decision = $leg.alternating; directory = $leg.directory }
            if ($leg.alternating.code -in @(1, 2)) {
                $enabled = @($group)
                $reproduced = $true
                break
            }
        }
        if (-not $reproduced) { return @{ status = 'no single-switch reproduction; interaction or noise'; attempts = $attempts } }
        if ($enabled.Count -eq 1) { return @{ status = 'reproduced'; switch = $enabled[0]; attempts = $attempts } }
    }
}

function Invoke-GateLeg($Configuration, [string]$Label, [string]$Kind, [int]$Seconds, [bool]$Measured, $Alternation = $null) {
    $directory = Join-Path $Output $Label
    $null = New-Item -ItemType Directory -Path $directory
    $result = @{ label = $Label; kind = $Kind; directory = $directory; status = 'running'; measured = $Measured; min_available_memory = $null }
    $script:ActiveLeg = $result
    $script:ActiveBlock = $null
    $script:ActiveBlocks = $null
    $upperBound = $null -ne $Alternation -and $Alternation.ContainsKey('experiment')
    if ($upperBound) { $result.stage = 'startup' }
    $script:AllLegs.Add($result)
    $configPath = Join-Path $directory 'configuration.json'
    Write-GateJson $configPath $Configuration
    $environment = Invoke-GatePython 'archb_gate_config.py' @('environment', $configPath, $directory, '--kind', $Kind)
    $recordedEnvironment = @{}
    foreach ($key in $environment.Keys) {
        if ($key -match '^(APS5_|ANYPS5_|VK_|VVL_)') { $recordedEnvironment[$key] = $environment[$key] }
    }
    Write-GateJson (Join-Path $directory 'environment.json') $recordedEnvironment
    Deploy-GateConfiguration $Configuration
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = Join-Path $Runtime 'demons-souls.exe'
    $start.WorkingDirectory = $Runtime
    $start.UseShellExecute = $false
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.Environment.Clear()
    foreach ($key in $environment.Keys) { $start.Environment[$key] = [string]$environment[$key] }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    $outStream = [IO.File]::Create((Join-Path $directory 'stdout.log'))
    $errStream = [IO.FileStream]::new((Join-Path $directory 'stderr.log'), [IO.FileMode]::Create,
        [IO.FileAccess]::Write, [IO.FileShare]::ReadWrite, 1, [IO.FileOptions]::Asynchronous)
    $lowSince = $null
    $navProcess = $null
    $outTask = $null
    $errTask = $null
    $started = $false
    $ticks = 0
    try {
        $started = $process.Start()
        if (-not $started) { Stop-GateFailure 30 'Cannot launch game' }
        $ticks = $process.StartTime.ToUniversalTime().Ticks
        $activePath = Join-Path $Output 'active-processes.json'
        $active = @{ game = @{ id = $process.Id; startTicks = $ticks } }
        Write-GateJson $activePath $active
        $outTask = $process.StandardOutput.BaseStream.CopyToAsync($outStream)
        $errTask = $process.StandardError.BaseStream.CopyToAsync($errStream)
        't_ns,private_commit,working_set,commit_headroom,available_memory' | Set-Content -LiteralPath (Join-Path $directory 'memory.csv') -Encoding utf8
        $navStart = [Diagnostics.ProcessStartInfo]::new()
        $navStart.FileName = $script:GatePython
        $navStart.UseShellExecute = $false
        foreach ($argument in @((Join-Path $script:GateTools 'archb_gate_nav.py'), 'navigate', '--pid', [string]$process.Id,
                                '--refs', $NavigationRefs, '--pad', (Join-Path $directory 'pad.txt'), '--output', $directory)) {
            $navStart.ArgumentList.Add($argument)
        }
        if ($upperBound) { $result.stage = 'navigation' }
        $navProcess = [Diagnostics.Process]::Start($navStart)
        $active.navigator = @{ id = $navProcess.Id; startTicks = $navProcess.StartTime.ToUniversalTime().Ticks }
        Write-GateJson $activePath $active
        while (-not $navProcess.WaitForExit(1000)) { $null = Add-GateMemorySample $process $directory ([ref]$lowSince) }
        if ($navProcess.ExitCode -ne 0) { Stop-GateFailure $navProcess.ExitCode 'Navigation failed' }
        if ($upperBound) { $result.stage = 'warmup' }
        $navigation = Get-Content -LiteralPath (Join-Path $directory 'navigation.json') -Raw | ConvertFrom-Json -AsHashtable
        $null = Invoke-GatePython 'archb_gate_nav.py' @('screenshot', '--pid', $process.Id, '--output', (Join-Path $directory 'start.png'))
        Wait-GateHold $process $directory $WarmupSeconds ([ref]$lowSince)
        $holdStart = Get-GateClock
        $stderrStart = $errStream.Position
        if ($upperBound) { $result.stage = 'blocks' }
        if ($null -ne $Alternation) { $result.alternating = Invoke-GateBlocks $process $directory $Alternation ([ref]$lowSince) }
        else { Wait-GateHold $process $directory $Seconds ([ref]$lowSince) }
        $holdEnd = Get-GateClock
        $stderrEnd = $errStream.Position
        Write-GateJson (Join-Path $directory 'hold.json') @{ start_ns = $holdStart; end_ns = $holdEnd
            stderr_start = $stderrStart; stderr_end = $stderrEnd }
        $memory = Add-GateMemorySample $process $directory ([ref]$lowSince)
        $modules = @($process.Modules | Where-Object { $_.FileName.EndsWith('.prx', [StringComparison]::OrdinalIgnoreCase) } | ForEach-Object {
            @{ path = $_.FileName; sha256 = (Get-FileHash -LiteralPath $_.FileName -Algorithm SHA256).Hash.ToLowerInvariant() }
        })
        Write-GateJson (Join-Path $directory 'modules.json') $modules
        $null = Invoke-GatePython 'archb_gate_nav.py' @('screenshot', '--pid', $process.Id, '--output', (Join-Path $directory 'end.png'))
        Stop-GateProcess $process.Id $ticks
        $outTask.GetAwaiter().GetResult()
        $errTask.GetAwaiter().GetResult()
        $outStream.Dispose()
        $errStream.Dispose()
        if ($upperBound) { $result.stage = 'verification' }
        $verifyArgs = @('verify', $configPath, $directory, $Runtime)
        if ($Kind -eq 'diagnostic' -and ($Configuration.name -ne $script:ControlName -or $script:AlternatingBootstrap)) { $verifyArgs += '--diagnostic' }
        $verification = Invoke-GatePython 'archb_gate_config.py' $verifyArgs
        $result.navigation = $navigation
        $result.memory = $memory
        $result.verification = $verification
        $result.hold_start_ns = $holdStart
        $result.hold_end_ns = $holdEnd
        $result.hold_seconds = if ($null -ne $Alternation) { ($holdEnd - $holdStart) / 1000000000.0 } else { $Seconds }
        if ($Measured -and $null -eq $Alternation) {
            $statistics = Invoke-GatePython 'archb_gate_stats.py' @('leg', (Join-Path $directory 'videoout.jsonl'), '--start-ns', $holdStart,
                                                                   '--end-ns', $holdEnd, '--hold-seconds', $Seconds) @(0, 27)
            foreach ($key in $statistics.Keys) { $result[$key] = $statistics[$key] }
        }
        if (($Measured -or $Kind -eq 'diagnostic') -and $Mode -ne 'Bootstrap') {
            $result.memory.incremental_host_bytes = [long]$memory.peak_private_commit - [long]$script:Baseline.peak_private_commit
            if (($Configuration.name -ne $script:ControlName -or $null -ne $Alternation) -and $result.memory.incremental_host_bytes -gt 4000000000) { Stop-GateFailure 26 'Incremental host memory exceeds 4,000,000,000 bytes' }
        }
        if ($Kind -eq 'diagnostic') {
            $result.attribution = Invoke-GatePython 'archb_gate_stats.py' @('diagnostic', $directory)
        }
        if ($Kind -eq 'validation') {
            $text = (Get-Content -LiteralPath (Join-Path $directory 'stderr.log') -Raw) + "`n" + (Get-Content -LiteralPath (Join-Path $directory 'stdout.log') -Raw)
            if ($text -match 'was not found but was requested by env var VK_INSTANCE_LAYERS') { Stop-GateFailure 25 'Requested validation layer is unavailable' }
            $result.validation_ids = @([regex]::Matches($text, '\b(?:VUID|SYNC)-[A-Za-z0-9_.-]+') | ForEach-Object { $_.Value } | Sort-Object -Unique)
            $result.validation_counts = @{ VUID = @($result.validation_ids | Where-Object { $_.StartsWith('VUID-') }).Count
                                           SYNC = @($result.validation_ids | Where-Object { $_.StartsWith('SYNC-') }).Count }
        }
        $result.status = 'completed'
        Write-GateJson (Join-Path $directory 'leg.json') $result
        return $result
    } finally {
        $script:ActiveBlock = $null
        $script:ActiveLeg = $null
        if ($null -ne $navProcess -and -not $navProcess.HasExited) { $navProcess.Kill(); $navProcess.WaitForExit() }
        if ($started) { Stop-GateProcess $process.Id $ticks }
        if ($null -ne $outTask) { $outTask.GetAwaiter().GetResult() }
        if ($null -ne $errTask) { $errTask.GetAwaiter().GetResult() }
        $outStream.Dispose()
        $errStream.Dispose()
        $process.Dispose()
        if ($null -ne $script:ActiveBlocks) {
            Write-GateJson (Join-Path $directory 'blocks.json') @($script:ActiveBlocks.ToArray())
            $script:ActiveBlocks = $null
        }
        if ($result.status -ne 'completed') {
            $result.status = 'failed'
            Write-GateJson (Join-Path $directory 'leg.json') $result
        }
    }
}

function Get-GateMedian($Values) {
    $ordered = @($Values | Sort-Object)
    $middle = [int][Math]::Floor($ordered.Count / 2)
    if ($ordered.Count % 2) { return $ordered[$middle] }
    return ($ordered[$middle - 1] + $ordered[$middle]) / 2
}

function Invoke-ArchBGate {
    $Output = [IO.Path]::GetFullPath($Output)
    $Runtime = [IO.Path]::GetFullPath($Runtime)
    $NavigationRefs = [IO.Path]::GetFullPath($NavigationRefs)
    $script:AllLegs = [Collections.Generic.List[object]]::new()
    $script:AlternatingBootstrap = $Mode -eq 'Bootstrap' -and $script:AllowAlternating
    $script:RestoreManifest = Join-Path $Output 'restore.json'
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $gate = @{ code = 30; verdict = 'incomplete'; mode = $Mode; legs = @(); restoration = @{ status = 'untouched' } }
    $mutex = [Threading.Mutex]::new($false, 'Global\AnyPS5GateRuntime')
    $owned = $false
    $manifestWritten = $false
    $outputCreated = $false
    try {
        try { $owned = $mutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $owned = $true }
        if (-not $owned) { Stop-GateFailure 30 'Another gate owns the runtime' }
        $busy = @(Get-Process | Where-Object { $_.ProcessName -in @('demons-souls', 'ninja', 'cmake', 'ctest', 'agc_frame_replay') })
        if ($busy.Count) { Stop-GateFailure 30 "Busy processes: $($busy.ProcessName -join ', ')" }
        $null = Invoke-GatePython 'archb_gate_config.py' @('repair', $Runtime)
        if ($Experiments -and ($Mode -ne 'Child' -or $Matrix -or $Bisect -or -not $script:AllowAlternating -or $BlockSeconds -ne 15 -or $SettleSeconds -ne 3)) { Stop-GateFailure 21 'Experiments require alternating Child mode, 15s blocks and 3s settling' }
        if (-not $Experiments -and (([bool]$Candidate) -eq ([bool]$Matrix))) { Stop-GateFailure 23 'Specify exactly one of -Candidate or -Matrix' }
        if ($SettleSeconds -ge $BlockSeconds) { Stop-GateFailure 24 'SettleSeconds must be less than BlockSeconds' }
        if ($Bisect -and ($Mode -notin @('Child', 'Milestone') -or -not $script:AllowAlternating -or $Matrix)) { Stop-GateFailure 21 'Bisection requires one alternating Child or Milestone candidate' }
        if (Test-Path -LiteralPath $Output) { Stop-GateFailure 30 'Output must be a new directory' }
        $runtimePath = [IO.Path]::GetFullPath($Runtime)
        $outputPath = [IO.Path]::GetFullPath($Output)
        $repoPath = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent))
        foreach ($forbidden in @($runtimePath, $repoPath)) {
            if ($outputPath.Equals($forbidden, [StringComparison]::OrdinalIgnoreCase) -or $outputPath.StartsWith($forbidden.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
                Stop-GateFailure 30 'Gate artifacts must be outside the runtime and repository'
            }
        }
        $null = New-Item -ItemType Directory -Path $Output
        $outputCreated = $true
        foreach ($path in @($Output, $Runtime)) {
            if ([IO.DriveInfo]::new([IO.Path]::GetPathRoot([IO.Path]::GetFullPath($path))).AvailableFreeSpace -lt ([int64]$MinFreeGB * 1GB)) { Stop-GateFailure 30 "At least $MinFreeGB GB free disk space is required" }
        }
        if ($Experiments -and -not $Candidate) {
            $selfCandidate = Get-Content -LiteralPath $Control -Raw | ConvertFrom-Json -AsHashtable
            $selfCandidate.name += '-ub-runner'
            $selfCandidate.shaderCache += '-ub-runner'
            $Candidate = Join-Path $Output 'ub-runner-candidate.json'
            Write-GateJson $Candidate $selfCandidate
        }
        $candidates = if ($Matrix) { @(Get-ChildItem -LiteralPath $Matrix -Filter '*.json' -File | Sort-Object Name | ForEach-Object { $_.FullName }) } else { @($Candidate) }
        $prepareArgs = @('prepare', '--control', $Control, '--mode', $Mode, '--output', (Join-Path $Output 'configurations.json'))
        foreach ($path in $candidates) { $prepareArgs += @('--candidate', $path) }
        $configuration = Invoke-GatePython 'archb_gate_config.py' $prepareArgs
        $script:ControlName = $configuration.control.name
        foreach ($item in @($configuration.control) + @($configuration.candidates)) {
            $cache = [IO.Path]::GetFullPath($item.shaderCache)
            foreach ($forbidden in @($runtimePath, $repoPath)) {
                if ($cache.Equals($forbidden, [StringComparison]::OrdinalIgnoreCase) -or
                    $cache.StartsWith($forbidden.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
                    Stop-GateFailure 23 'Shader caches must be outside the runtime and repository'
                }
            }
            if ($cache -match '(?i)(^|[\\/])app0([\\/]|$)') { Stop-GateFailure 23 'A shader cache cannot be inside app0' }
            if ([IO.DriveInfo]::new([IO.Path]::GetPathRoot($cache)).AvailableFreeSpace -lt ([int64]$MinFreeGB * 1GB)) { Stop-GateFailure 30 "Shader cache drive needs at least $MinFreeGB GB free" }
        }
        if ($Mode -eq 'Final' -and $HoldSeconds -ne 120) { Stop-GateFailure 24 'Final legs must hold for 120s' }
        $baselinePath = Join-Path (Split-Path $Output -Parent) 'gate-baseline.json'
        $script:BaselinePath = $baselinePath
        if ($Mode -eq 'Bootstrap' -and (Test-Path -LiteralPath $baselinePath)) { Stop-GateFailure 30 'Frozen baseline already exists; use a new parent directory for a new Bootstrap' }
        if ($Mode -ne 'Bootstrap' -and -not $Experiments) {
            $script:Baseline = Get-Content -LiteralPath $baselinePath -Raw | ConvertFrom-Json -AsHashtable
            $gate.baseline = $script:Baseline
            if (1.677927 * $script:Baseline.s_AA / [Math]::Sqrt(48) -gt 0.01) { Stop-GateFailure 40 'Frozen Bootstrap noise cannot resolve 2% at 48 pairs' }
            if (-not $script:Baseline.ContainsKey('mean_ms') -or $script:Baseline.mean_ms -le 0) { Stop-GateFailure 24 'Frozen Bootstrap needs its presented-frame mean_ms' }
            $referenceHashes = @{}
            foreach ($reference in Get-ChildItem -LiteralPath $NavigationRefs -Filter '*.bmp' -File) { $referenceHashes[$reference.Name] = (Get-FileHash -LiteralPath $reference.FullName).Hash.ToLowerInvariant() }
            if ((($referenceHashes.Keys | Sort-Object) -join ',') -ne (($script:Baseline.navref_hashes.Keys | Sort-Object) -join ',')) { Stop-GateFailure 11 'Navigation reference set differs from frozen Bootstrap' }
            foreach ($name in $script:Baseline.navref_hashes.Keys) {
                if ($referenceHashes[$name] -ne $script:Baseline.navref_hashes[$name]) { Stop-GateFailure 11 'Navigation reference hashes differ from frozen Bootstrap' }
            }
        }
        Initialize-GateMemory
        $display = Get-GateDisplay
        $manifest = @{ configurations = $configuration; started_utc = [DateTime]::UtcNow.ToString('o')
                       gpu = @(Get-CimInstance Win32_VideoController | Select-Object Name, DriverVersion, DriverDate)
                       display = $display
                       power_plan = (& powercfg /getactivescheme | Out-String).Trim()
                       windows = (Get-CimInstance Win32_OperatingSystem | Select-Object Caption, Version, BuildNumber)
                       warmup_seconds = $WarmupSeconds; hold_seconds = $HoldSeconds
                       min_available_mib = $MinAvailableMiB; low_memory_seconds = $LowMemorySeconds
                       alternating_allowed = $script:AllowAlternating; block_seconds = $BlockSeconds; settle_seconds = $SettleSeconds }
        Write-GateJson (Join-Path $Output 'manifest.json') $manifest
        $libraries = @(@($configuration.control.libs.Keys) + @($configuration.candidates | ForEach-Object { $_.libs.Keys }) | Sort-Object -Unique)
        $null = Invoke-GatePython 'archb_gate_config.py' (@('snapshot', $Runtime, $script:RestoreManifest) + $libraries)
        $manifestWritten = $true
        $runner = Get-Process -Id $PID
        $null = Start-GateWatchdog $script:RestoreManifest $PID $runner.StartTime.ToUniversalTime().Ticks
        if ($Experiments) {
            $gate.experiments = @(Invoke-GateUpperBoundExperiments $configuration.control)
            $gate.code = 0
            $gate.verdict = 'experiments reported'
        } else {
        if ($script:AlternatingBootstrap) {
            $controlWarmup = Invoke-GateLeg $configuration.control 'bootstrap-navigation-1-diagnostic' 'diagnostic' 60 $false
            $candidateWarmups = @{}
            $candidateWarmups[$configuration.candidates[0].name] = Invoke-GateLeg $configuration.candidates[0] 'bootstrap-navigation-2-validation' 'validation' 60 $false
        } else {
            if ($Mode -eq 'Bootstrap') { $gate.bootstrap_diagnostic = Invoke-GateLeg $configuration.candidates[0] 'bootstrap-diagnostic' 'diagnostic' 1 $false }
            $controlWarmup = Invoke-GateLeg $configuration.control "warmup-control-$($configuration.control.name)" 'measured' 60 $false
            $candidateWarmups = @{}
            foreach ($candidateConfig in $configuration.candidates) {
                $candidateWarmups[$candidateConfig.name] = Invoke-GateLeg $candidateConfig "warmup-candidate-$($candidateConfig.name)" 'measured' 60 $false
            }
        }
        $results = @()
        $gate.alternating_plans = @{}
        $allAlternating = $true
        foreach ($candidateConfig in $configuration.candidates) {
            $pairs = @()
            $plan = Get-GateAlternatingPlan $controlWarmup $candidateWarmups[$candidateConfig.name]
            $useAlternating = $script:AllowAlternating -and $plan.eligible -and $Mode -ne 'Final'
            $gate.alternating_plans[$candidateConfig.name] = $plan
            if ($Bisect -and -not $useAlternating) { Stop-GateFailure 21 'Bisection requires identical builds and runtime-only differences' }
            if ($script:AlternatingBootstrap -and -not $useAlternating) { Stop-GateFailure 21 'Alternating Bootstrap requires runtime GATE_NOOP' }
            if (-not $useAlternating) { $allAlternating = $false }
            if ($Mode -eq 'Final') {
                $legs = @(1..3 | ForEach-Object { Invoke-GateLeg $candidateConfig "$($candidateConfig.name)-final-$_" 'measured' 120 $true })
                $pairsPath = Join-Path $Output "$($candidateConfig.name)-final.json"
                Write-GateJson $pairsPath $legs
                $decision = Invoke-GatePython 'archb_gate_stats.py' @('decide', $pairsPath, '--mode', 'Final', '--baseline', $baselinePath) @(0, 1, 27)
            } elseif ($useAlternating) {
                if ($Mode -ne 'Bootstrap' -and -not $script:Baseline.ContainsKey('alternating_m')) { Stop-GateFailure 40 'An alternating Bootstrap baseline is required' }
                $label = if ($Mode -eq 'Bootstrap') { 'bootstrap-navigation-3-alternating' } else { "$($candidateConfig.name)-alternating" }
                $alternatingLeg = Invoke-GateLeg $configuration.control $label 'measured' 0 $true $plan
                $decision = $alternatingLeg.alternating
                if ($Bisect -and $decision.code -in @(1, 2)) { $gate.bisection = Invoke-GateBisection $candidateConfig $plan }
            } else {
                if ($Mode -ne 'Bootstrap' -and -not $script:Baseline.ContainsKey('m')) { Stop-GateFailure 40 'A paired Bootstrap baseline is required for paired boots' }
                $maxPairs = if ($Mode -eq 'Bootstrap') { 6 } else { 8 }
                for ($index = 1; $index -le $maxPairs; ++$index) {
                    $pair = @{}
                    $order = if ($index % 2) { @('control', 'candidate') } else { @('candidate', 'control') }
                    foreach ($role in $order) {
                        $item = if ($role -eq 'control') { $configuration.control } else { $candidateConfig }
                        $pair[$role] = Invoke-GateLeg $item "$($candidateConfig.name)-pair-$index-$role" 'measured' $HoldSeconds $true
                    }
                    $pairs += $pair
                    $pairsPath = Join-Path $Output "$($candidateConfig.name)-pairs.json"
                    Write-GateJson $pairsPath $pairs
                    if ($Mode -eq 'Bootstrap' -and $index -ne 6) { continue }
                    if ($Mode -ne 'Bootstrap' -and $index -notin @(4, 8)) { continue }
                    $decideArgs = @('decide', $pairsPath, '--mode', $Mode)
                    if ($Mode -ne 'Bootstrap') { $decideArgs += @('--baseline', $baselinePath) }
                    $decision = Invoke-GatePython 'archb_gate_stats.py' $decideArgs @(0, 1, 2, 3, 27, 40)
                    if ($decision.verdict -ne 'continue') { break }
                }
            }
            $decision.candidate = $candidateConfig.name
            $results += $decision
            $gate.results = $results
        }
        $diagnostics = @{}
        if ($script:AlternatingBootstrap) {
            $diagnostics[$configuration.candidates[0].name] = $controlWarmup
        } else {
            $diagnosticConfigurations = if ($allAlternating) { @($configuration.candidates) } else { @($configuration.control) + @($configuration.candidates) }
            foreach ($item in $diagnosticConfigurations) { $diagnostics[$item.name] = Invoke-GateLeg $item "diagnostic-$($item.name)" 'diagnostic' 60 $false }
        }
        $gate.diagnostics = $diagnostics
        $gate.validation_diffs = @{}
        $cacheHash = (Invoke-GatePython 'archb_gate_config.py' @('validation-key', (Join-Path $Output 'manifest.json'))).key
        $validationCachePath = Join-Path (Split-Path $Output -Parent) "validation-control-$cacheHash.json"
        if ($script:AlternatingBootstrap) {
            $controlValidation = $candidateWarmups[$configuration.candidates[0].name]
            $gate.validation_diffs[$configuration.candidates[0].name] = @{ added = @(); control = $controlValidation.validation_ids; candidate = $controlValidation.validation_ids }
            $log = Join-Path $controlValidation.directory 'stderr.log'
            Write-GateJson $validationCachePath @{ key = $cacheHash; leg = $controlValidation; log = $log; log_sha256 = (Get-FileHash -LiteralPath $log).Hash }
        } else {
            $cached = if (Test-Path -LiteralPath $validationCachePath) { Get-Content -LiteralPath $validationCachePath -Raw | ConvertFrom-Json -AsHashtable } else { $null }
            if ($allAlternating -and $null -ne $cached -and (Test-Path -LiteralPath $cached.log) -and
                (Get-FileHash -LiteralPath $cached.log).Hash -eq $cached.log_sha256) {
                $controlValidation = $cached.leg
                $gate.control_validation_cache = $validationCachePath
            } else {
                $controlValidation = Invoke-GateLeg $configuration.control "validation-control-$($configuration.control.name)" 'validation' 60 $false
                if ($allAlternating) {
                    $log = Join-Path $controlValidation.directory 'stderr.log'
                    Write-GateJson $validationCachePath @{ key = $cacheHash; leg = $controlValidation; log = $log; log_sha256 = (Get-FileHash -LiteralPath $log).Hash }
                }
            }
            foreach ($item in $configuration.candidates) {
                $validation = Invoke-GateLeg $item "validation-candidate-$($item.name)" 'validation' 60 $false
                $new = @($validation.validation_ids | Where-Object { $_ -notin $controlValidation.validation_ids })
                $gate.validation_diffs[$item.name] = @{ added = $new; control = $controlValidation.validation_ids; candidate = $validation.validation_ids
                                                      control_log = (Join-Path $controlValidation.directory 'stderr.log') }
                if ($new.Count) { Stop-GateFailure 25 "New validation identifiers: $($new -join ', ')" }
            }
        }
        $gate.code = 0
        foreach ($result in $results) { if ($result.code -ne 0) { $gate.code = $result.code; break } }
        $gate.verdict = if ($gate.code -eq 0) { 'pass' } elseif ($gate.code -eq 3) { 'inconclusive' } elseif ($gate.code -eq 27) { 'present_drops' } else { 'fail' }
        if ($Mode -eq 'Bootstrap' -and $gate.code -eq 0) {
            $firstNav = $script:AllLegs[0].navigation
            $baseline = @{ s_AA = $results[0].s_AA; d_i = $results[0].d_i; mean_ms = $results[0].mean_ms
                           peak_private_commit = Get-GateMedian @($script:AllLegs | Where-Object { $_.ContainsKey('ms') -or $_.ContainsKey('alternating') } | ForEach-Object { $_.memory.peak_private_commit })
                           T_nav = @($script:AllLegs | ForEach-Object { $_.navigation.T_nav }); navref_hashes = $firstNav.reference_hashes
                           manifest = (Join-Path $Output 'manifest.json'); contact = (Join-Path $Output 'contact.png') }
            if ($script:AlternatingBootstrap) { $baseline.alternating_m = $results[0].alternating_m }
            else { $baseline.m = $results[0].m }
            Write-GateJson (Join-Path $Output 'gate-baseline.json') $baseline
            $gate.baseline = $baseline
        }
        }
    } catch {
        $gate.code = if ($_.Exception.Data.Contains('GateCode')) { [int]$_.Exception.Data['GateCode'] } else { 30 }
        $gate.verdict = 'fail'
        $gate.error = $_.Exception.Message
        [Console]::Error.WriteLine($_.Exception.Message)
    } finally {
        if ($manifestWritten) {
            try { $gate.restoration = Restore-GateRuntime $script:RestoreManifest }
            catch { $gate.code = 31; $gate.restoration = @{ status = 'failed'; error = $_.Exception.Message } }
        }
        $gate.legs = @($script:AllLegs.ToArray())
        $gate.verdict_notes = @(@($gate.results) + @($gate.experiments) | ForEach-Object { $_.verdict_notes } | Sort-Object -Unique)
        $gate.wall_clock_seconds = $watch.Elapsed.TotalSeconds
        if ($outputCreated) {
            try { $null = Invoke-GatePython 'archb_gate_nav.py' @('contact', $Output) }
            catch {
                $gate.contact_error = $_.Exception.Message
                if ($gate.code -eq 0) { $gate.code = 30; $gate.verdict = 'fail' }
            }
            if ($gate.code -eq 0 -and $Mode -eq 'Bootstrap' -and $gate.restoration.status -eq 'restored') {
                try { [IO.File]::Copy((Join-Path $Output 'gate-baseline.json'), $baselinePath, $false) }
                catch { $gate.code = 30; $gate.verdict = 'fail'; $gate.error = $_.Exception.Message }
            }
            $gate.wall_clock_seconds = $watch.Elapsed.TotalSeconds
            Write-GateJson (Join-Path $Output 'gate.json') $gate
        }
        if ($owned) { $mutex.ReleaseMutex() }
        $mutex.Dispose()
    }
    return $gate.code
}

if ($MyInvocation.InvocationName -ne '.') {
    if ($PSCmdlet.ParameterSetName -eq 'Watchdog') {
        try { Invoke-GateWatchdog $WatchdogManifest $RunnerPid $RunnerStartTicks }
        catch { [Console]::Error.WriteLine($_.Exception.Message); exit 31 }
    }
    else { exit (Invoke-ArchBGate) }
}
