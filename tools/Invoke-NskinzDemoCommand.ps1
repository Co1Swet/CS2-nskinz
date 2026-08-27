param(
    [Parameter(Mandatory = $true)]
    [string]$Command,

    [int]$TargetProcessId = 0,

    [string]$ModuleDirectory = '',

    [int]$WaitMilliseconds = 750,

    [switch]$WaitForDispatch,

    [int]$DispatchTimeoutSeconds = 15
)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class NSkinzDemoCommandNative
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

$trimmedCommand = $Command.Trim()
if ([string]::IsNullOrWhiteSpace($trimmedCommand)) {
    throw 'Demo command must not be empty.'
}

$commandName = ($trimmedCommand -split '\s+', 2)[0].ToLowerInvariant()
if ($commandName -in @('demo_gototick', 'playdemo')) {
    throw "$commandName performs a synchronous demo/full-packet transition and must be sent through NetCon before nSkinz is injected; dispatching it from FRAME_RENDER_PASS is unsafe."
}

if ([string]::IsNullOrWhiteSpace($ModuleDirectory)) {
    $process = Get-Process -Id $TargetProcessId -ErrorAction Stop
    $module = $process.Modules |
        Where-Object { $_.ModuleName -ieq 'nSkinzCS2.dll' } |
        Select-Object -First 1
    if ($null -eq $module) {
        throw "nSkinzCS2.dll was not found in PID $TargetProcessId; pass -ModuleDirectory explicitly."
    }
    $ModuleDirectory = Split-Path -Parent $module.FileName
}

$resolvedModuleDirectory = (Resolve-Path -LiteralPath $ModuleDirectory).Path
$sidecarPath = Join-Path $resolvedModuleDirectory 'nSkinzCS2.demo-command.txt'
$logPath = Join-Path $resolvedModuleDirectory 'nSkinzCS2.log'
$initialLogLength = if (Test-Path -LiteralPath $logPath) {
    (Get-Item -LiteralPath $logPath).Length
}
else {
    0
}
$utf8WithoutBom = [Text.UTF8Encoding]::new($false)
[IO.File]::WriteAllText($sidecarPath, $trimmedCommand + "`r`n", $utf8WithoutBom)

$eventName = "Local\nSkinzCS2.DemoCommand.$TargetProcessId"
$eventModifyState = 0x0002
$eventHandle = [NSkinzDemoCommandNative]::OpenEvent(
    $eventModifyState,
    $false,
    $eventName)
if ($eventHandle -eq [IntPtr]::Zero) {
    throw "nSkinzCS2 local demo command event was not found for PID $TargetProcessId."
}

try {
    if (-not [NSkinzDemoCommandNative]::SetEvent($eventHandle)) {
        throw "SetEvent failed for nSkinzCS2 PID $TargetProcessId."
    }

    if ($WaitForDispatch) {
        $expectedLogText = "Local demo automation dispatched '$trimmedCommand'"
        $deadline = (Get-Date).AddSeconds($DispatchTimeoutSeconds)
        $observedText = ''
        do {
            Start-Sleep -Milliseconds 50
            if (Test-Path -LiteralPath $logPath) {
                $stream = [IO.File]::Open(
                    $logPath,
                    [IO.FileMode]::Open,
                    [IO.FileAccess]::Read,
                    [IO.FileShare]::ReadWrite)
                try {
                    if ($stream.Length -gt $initialLogLength) {
                        [void]$stream.Seek($initialLogLength, [IO.SeekOrigin]::Begin)
                        $reader = [IO.StreamReader]::new($stream, [Text.Encoding]::UTF8, $true, 4096, $true)
                        try {
                            $observedText = $reader.ReadToEnd()
                        }
                        finally {
                            $reader.Dispose()
                        }
                    }
                }
                finally {
                    $stream.Dispose()
                }
            }
        } while (-not $observedText.Contains($expectedLogText) -and (Get-Date) -lt $deadline)

        if (-not $observedText.Contains($expectedLogText)) {
            throw "Timed out waiting for nSkinzCS2 to dispatch '$trimmedCommand'."
        }
        "DEMO_COMMAND_DISPATCHED pid=$TargetProcessId command=$trimmedCommand"
    }
    else {
        Start-Sleep -Milliseconds $WaitMilliseconds
        "DEMO_COMMAND_REQUESTED pid=$TargetProcessId command=$trimmedCommand"
    }
}
finally {
    [void][NSkinzDemoCommandNative]::CloseHandle($eventHandle)
}
