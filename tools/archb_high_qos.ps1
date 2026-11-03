param([Parameter(Mandatory)][int]$GamePid)
# Tag one game process (one this tooling launched) High QoS: opt it out of execution-speed throttling, keeping the timer-resolution
# opt-out libkernel sets (PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION with state 0). Windows classes a foreground window's
# threads High QoS; a game launched while the interactive session is disconnected runs as a background window at Medium QoS
# (2026-11-03: every CSwitch of the game "Important" at ~60 ms/f in the morning, "MediumImportant" at ~72 ms/f once the RDP
# session had disconnected). Prints the state read back: control/state masks.
if (-not ('ZooQos' -as [type])) {
    Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public static class ZooQos {
  [StructLayout(LayoutKind.Sequential)] public struct Throttling { public uint Version; public uint ControlMask; public uint StateMask; }
  [DllImport("kernel32.dll", SetLastError = true)] public static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
  [DllImport("kernel32.dll", SetLastError = true)] public static extern bool CloseHandle(IntPtr handle);
  [DllImport("kernel32.dll", SetLastError = true)] public static extern bool SetProcessInformation(IntPtr process, int infoClass, ref Throttling info, uint size);
  [DllImport("kernel32.dll", SetLastError = true)] public static extern bool GetProcessInformation(IntPtr process, int infoClass, ref Throttling info, uint size);
}
"@
}
$handle = [ZooQos]::OpenProcess(0x0200 -bor 0x0400, $false, $GamePid)
if ($handle -eq [IntPtr]::Zero) { throw "OpenProcess($GamePid) failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())" }
try {
    $state = [ZooQos+Throttling]::new(); $state.Version = 1; $state.ControlMask = 0x5; $state.StateMask = 0
    $size = [uint32][Runtime.InteropServices.Marshal]::SizeOf([type][ZooQos+Throttling])
    if (-not [ZooQos]::SetProcessInformation($handle, 4, [ref]$state, $size)) { throw "SetProcessInformation failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())" }
    $back = [ZooQos+Throttling]::new(); $back.Version = 1
    $null = [ZooQos]::GetProcessInformation($handle, 4, [ref]$back, $size)
    "high-qos pid $GamePid control=0x{0:x} state=0x{1:x}" -f $back.ControlMask, $back.StateMask
} finally { $null = [ZooQos]::CloseHandle($handle) }
