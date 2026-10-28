param(
    [Parameter(Mandatory)][string]$Case,
    [switch]$Child,
    [string]$Root
)

$ErrorActionPreference = 'Stop'
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../../../'))
$runnerScript = Join-Path $repository 'tools/archb_gate.ps1'
. $runnerScript -Control unused -Output unused

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Set-FakeBytes([string]$Path, [byte[]]$Bytes) {
    [IO.File]::WriteAllBytes($Path, $Bytes)
}

function Save-FakeRuntime([string]$Directory) {
    $runtime = Join-Path $Directory 'runtime'
    $manifest = Join-Path $Directory 'restore.json'
    $null = Invoke-GatePython 'archb_gate_config.py' @('snapshot', $runtime, $manifest, 'libSceAgcDriver.prx', 'libSceVideoOut.prx')
    return $manifest
}

function Change-FakeRuntime([string]$Directory) {
    foreach ($path in @('libs/libSceAgcDriver.prx', 'libs/unpatched/libSceAgcDriver.prx', 'libs/libSceVideoOut.prx', 'libs/unpatched/libSceVideoOut.prx', 'app0/PackageCmdLineArgs.txt')) {
        Set-FakeBytes (Join-Path $Directory "runtime/$path") ([byte[]]@(7, 8, 9))
    }
}

if ($Case -eq 'AlternatingBlocks' -or $Case -eq 'Bisection') {
    $testRoot = Join-Path $repository ('build/archb-alternating-' + [Guid]::NewGuid().ToString('N'))
    $null = New-Item -ItemType Directory -Path $testRoot
    try {
        if ($Case -eq 'AlternatingBlocks') {
            $stderrPath = Join-Path $testRoot 'stderr.log'
            [IO.File]::WriteAllText($stderrPath, "[archb] toggle seq=1 t_ns=123 GATE_NOOP=1 GATE_DELAY_US=0`n")
            Assert-True ((Wait-GateToggle @{ HasExited = $false } $testRoot 1) -eq 123) 'Toggle acknowledgement was not read'
            [IO.File]::WriteAllText($stderrPath, "[archb] toggle refused seq=2 unknown FOO`n")
            $refused = $false
            try { $null = Wait-GateToggle @{ HasExited = $false } $testRoot 2 }
            catch { $refused = $_.Exception.Data['GateCode'] -eq 21 }
            Assert-True $refused 'Refused toggle did not fail with code 21'
            $Mode = 'Child'
            $BlockSeconds = 1
            $script:fakeClock = 1000000000L
            $script:screenshots = @()
            function Get-GateClock { $script:fakeClock += 1000000000; return $script:fakeClock }
            function Add-GateMemorySample { param($Process, $Directory, $LowSince) }
            function Wait-GateToggle {
                param($Process, $Directory, $Sequence)
                $line = [IO.File]::ReadAllText((Join-Path $Directory 'control.txt'))
                Assert-True ($line.StartsWith("$Sequence ")) 'Control sequence differs from acknowledgement request'
                Assert-True ($line -notmatch '[^\x00-\x7f]') 'Control file is not ASCII'
                return Get-GateClock
            }
            function Invoke-GatePython {
                param($Tool, $Arguments, $AllowedCodes)
                if ($Tool -eq 'archb_gate_nav.py') { $script:screenshots += $Arguments[-1]; return @{} }
                $blocks = Get-Content -LiteralPath (Join-Path $testRoot 'blocks.json') -Raw | ConvertFrom-Json -AsHashtable
                return @{ verdict = $(if ($blocks.Count -eq 24) { 'continue' } else { 'pass' }); code = 0; stage = $blocks.Count / 2 }
            }
            $low = $null
            $decision = Invoke-GateBlocks @{} $testRoot @{ control = @{ GATE_NOOP = '0'; GATE_DELAY_US = '0' }
                candidate = @{ GATE_NOOP = '1'; GATE_DELAY_US = '7000' } } ([ref]$low)
            Assert-True ($decision.stage -eq 24) 'Alternation did not extend to 24 pairs'
            $blocks = Get-Content -LiteralPath (Join-Path $testRoot 'blocks.json') -Raw | ConvertFrom-Json -AsHashtable
            Assert-True ($blocks.Count -eq 48) 'Wrong block count'
            for ($i = 0; $i -lt $blocks.Count; $i += 2) {
                Assert-True ($blocks[$i].role -ne $blocks[$i + 1].role) 'Pair lacks a control or candidate'
                Assert-True ($blocks[$i].end_seq -eq $blocks[$i + 1].start_seq) 'Pair boundary sequence differs'
            }
            Assert-True ($script:screenshots.Count -eq 4) 'Missing end-of-stage role screenshots'
        } else {
            $script:attempts = @()
            function Invoke-GateLeg {
                param($Configuration, $Label, $Kind, $Seconds, $Measured, $Alternation)
                Assert-True (@($Alternation.control.Values | Where-Object { $_ -ne '0' }).Count -eq 0) 'Bisection control is not all off'
                $script:attempts += $Alternation
                return @{ directory = $testRoot; alternating = @{ code = $(if ($Alternation.candidate.GATE_NOOP -eq '1') { 1 } else { 0 }) } }
            }
            $configuration = @{ name = 'test'; env = @{}; expectSwitches = @{} }
            $plan = @{ control = @{ GATE_NOOP = '0'; GATE_DELAY_US = '0' }; candidate = @{ GATE_NOOP = '1'; GATE_DELAY_US = '7000' }
                metadata = @{ GATE_RUN = @{ value = '1'; runtime = $false }; GATE_NOOP = @{ value = '1'; runtime = $true }
                              GATE_DELAY_US = @{ value = '7000'; runtime = $true } } }
            $result = Invoke-GateBisection $configuration $plan
            Assert-True ($result.status -eq 'reproduced' -and $result.switch -eq 'GATE_NOOP') 'Bisection did not isolate the failing switch'
            Assert-True ($script:attempts.Count -le 3) 'Bisection repeated unnecessary boots'
        }
        Write-Output "$Case passed"
    } finally {
        $resolved = [IO.Path]::GetFullPath($testRoot)
        $allowed = [IO.Path]::GetFullPath((Join-Path $repository 'build')).TrimEnd('\') + '\'
        if (-not $resolved.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe alternating test cleanup path' }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
    exit 0
}

if ($Child) {
    $manifest = Save-FakeRuntime $Root
    Change-FakeRuntime $Root
    $notBefore = [DateTime]::UtcNow.AddSeconds(-1).Ticks
    Set-FakeBytes (Join-Path $Root 'runtime/frame_000.bmp') ([byte[]]@(66, 77, 0, 255))
    Write-GateJson (Join-Path $Root 'active-processes.json') @{ capture = @{
        runtime = (Join-Path $Root 'runtime'); destination = (Join-Path $Root 'captured'); notBeforeTicks = $notBefore
    } }
    $self = Get-Process -Id $PID
    $null = Start-GateWatchdog $manifest $PID $self.StartTime.ToUniversalTime().Ticks
    Set-Content -LiteralPath (Join-Path $Root 'ready') -Value ready
    while ($true) { Start-Sleep -Seconds 1 }
}

$buildRoot = [IO.Path]::GetFullPath((Join-Path $repository 'build'))
$Root = Join-Path $buildRoot ('archb-restore-' + [Guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path (Join-Path $Root 'runtime/libs/unpatched')
$null = New-Item -ItemType Directory -Path (Join-Path $Root 'shared-app0')
$junction = Join-Path $Root 'runtime/app0'
$null = New-Item -ItemType Junction -Path $junction -Target (Join-Path $Root 'shared-app0')
$originals = @{
    'libs/libSceAgcDriver.prx' = [byte[]]@(0, 1, 255, 0)
    'libs/unpatched/libSceAgcDriver.prx' = [byte[]]@(255, 0, 2, 3)
    'libs/libSceVideoOut.prx' = [byte[]]@(4, 5, 0, 255)
    'libs/unpatched/libSceVideoOut.prx' = [byte[]]@(6, 0, 255, 7)
    'app0/PackageCmdLineArgs.txt' = [byte[]]@(97, 0, 114, 103, 115, 13, 10)
}
$tracked = $null
try {
    foreach ($path in $originals.Keys) { Set-FakeBytes (Join-Path $Root "runtime/$path") $originals[$path] }
    if ($Case -eq 'RestoreMissingArgsFile') {
        Remove-Item -LiteralPath (Join-Path $Root 'runtime/app0/PackageCmdLineArgs.txt')
        $originals.Remove('app0/PackageCmdLineArgs.txt')
    }
    if ($Case -eq 'WatchdogRestoresAfterKill') {
        $arguments = @('-NoProfile', '-File', "`"$PSCommandPath`"", '-Case', $Case, '-Child', '-Root', "`"$Root`"")
        $tracked = Start-Process -FilePath (Get-Command pwsh).Source -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardError (Join-Path $Root 'child-stderr.log')
        $until = [DateTime]::UtcNow.AddSeconds(10)
        while (-not (Test-Path -LiteralPath (Join-Path $Root 'ready')) -and [DateTime]::UtcNow -lt $until -and -not $tracked.HasExited) { Start-Sleep -Milliseconds 100 }
        Assert-True (Test-Path -LiteralPath (Join-Path $Root 'ready')) 'Child did not initialize watchdog'
        Stop-GateProcess $tracked.Id $tracked.StartTime.ToUniversalTime().Ticks
        $manifest = Join-Path $Root 'restore.json'
        $until = [DateTime]::UtcNow.AddSeconds(10)
        do {
            Start-Sleep -Milliseconds 100
            $plan = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
        } while ($plan.status -ne 'restored' -and [DateTime]::UtcNow -lt $until)
        Assert-True ($plan.status -eq 'restored') 'Watchdog did not restore after kill'
        $until = [DateTime]::UtcNow.AddSeconds(2)
        while (-not (Test-Path -LiteralPath (Join-Path $Root 'gate.json')) -and [DateTime]::UtcNow -lt $until) { Start-Sleep -Milliseconds 100 }
        Assert-True (Test-Path -LiteralPath (Join-Path $Root 'captured/frame_000.bmp')) 'Watchdog did not collect its diagnostic dump'
        Assert-True (-not (Test-Path -LiteralPath (Join-Path $Root 'runtime/frame_000.bmp'))) 'Diagnostic dump was left in fake runtime'
    } else {
        $manifest = Save-FakeRuntime $Root
        Change-FakeRuntime $Root
        if ($Case -eq 'RejectWrongPidStartTime') {
            $tracked = Start-Process -FilePath (Get-Command pwsh).Source -ArgumentList @('-NoProfile', '-Command', 'Start-Sleep -Seconds 20') -WindowStyle Hidden -PassThru
            $ticks = $tracked.StartTime.ToUniversalTime().Ticks
            $rejected = $false
            try { Stop-GateProcess $tracked.Id ($ticks + 1) }
            catch { $rejected = $_.Exception.Data['GateCode'] -eq 30 }
            Assert-True $rejected 'Wrong PID start time was not rejected'
            Assert-True (Test-GateProcess $tracked.Id $ticks) 'Wrong PID identity killed a process'
            $null = Restore-GateRuntime $manifest
        } else {
            $failureCode = 0
            try {
                try { Stop-GateFailure 11 'Synthetic navigation failure' }
                finally { $null = Restore-GateRuntime $manifest }
            } catch { $failureCode = $_.Exception.Data['GateCode'] }
            Assert-True ($failureCode -eq 11) 'Navigation failure lost its distinct exit code'
        }
    }
    foreach ($path in $originals.Keys) {
        $actual = [Convert]::ToBase64String([IO.File]::ReadAllBytes((Join-Path $Root "runtime/$path")))
        Assert-True ($actual -eq [Convert]::ToBase64String($originals[$path])) "Bytes differ after restore: $path"
    }
    if ($Case -eq 'RestoreMissingArgsFile') { Assert-True (-not (Test-Path -LiteralPath (Join-Path $Root 'runtime/app0/PackageCmdLineArgs.txt'))) 'Originally missing args file was left behind' }
    Assert-True ((Get-Item -LiteralPath $junction).LinkType -eq 'Junction') 'Restore replaced the app0 junction'
    Write-Output "$Case passed"
} finally {
    if ($null -ne $tracked -and -not $tracked.HasExited) { Stop-GateProcess $tracked.Id $tracked.StartTime.ToUniversalTime().Ticks }
    $resolvedRoot = [IO.Path]::GetFullPath($Root)
    if (-not $resolvedRoot.StartsWith($buildRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe test cleanup path' }
    if (Test-Path -LiteralPath $junction) { Remove-Item -LiteralPath $junction -Force }
    Remove-Item -LiteralPath $resolvedRoot -Recurse -Force
}
