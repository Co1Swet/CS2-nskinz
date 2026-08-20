param(
    [int]$TargetProcessId = 0,
    [int]$WaitMilliseconds = 750
)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class NSkinzUpdateNative
{
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr OpenEvent(uint desiredAccess, bool inheritHandle, string name);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool SetEvent(IntPtr handle);
    [DllImport("kernel32.dll")]
    public static extern bool CloseHandle(IntPtr handle);
}
'@

if ($TargetProcessId -eq 0) {
    $processes = @(Get-Process -Name cs2 -ErrorAction Stop)
    if ($processes.Count -ne 1) {
        throw "Expected exactly one CS2 process, found $($processes.Count)."
    }
    $TargetProcessId = $processes[0].Id
}

$eventName = "Local\nSkinzCS2.ManualRefresh.$TargetProcessId"
$eventModifyState = 0x0002
$eventHandle = [NSkinzUpdateNative]::OpenEvent(
    $eventModifyState,
    $false,
    $eventName)
if ($eventHandle -eq [IntPtr]::Zero) {
    throw "nSkinzCS2 local regression refresh event was not found for PID $TargetProcessId."
}

try {
    if (-not [NSkinzUpdateNative]::SetEvent($eventHandle)) {
        throw "SetEvent failed for nSkinzCS2 PID $TargetProcessId."
    }
    Start-Sleep -Milliseconds $WaitMilliseconds
    "UPDATE_REQUESTED pid=$TargetProcessId event=$eventName"
}
finally {
    [void][NSkinzUpdateNative]::CloseHandle($eventHandle)
}
