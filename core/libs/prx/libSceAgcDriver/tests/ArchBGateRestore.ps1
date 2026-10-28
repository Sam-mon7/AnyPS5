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
