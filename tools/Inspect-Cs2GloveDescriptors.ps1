param(
    [int]$ProcessId = 0,
    [UInt64]$TableBeginRva = 0x2317B90,
    [UInt64]$TableEndRva = 0x23189F0,
    [int]$RecordSize = 0x28,
    [switch]$ExpandField18,
    [UInt64]$DumpRva = 0,
    [UInt64]$DumpAddress = 0,
    [UInt64]$CompareAddress = 0,
    [int]$DumpLength = 0x100,
    [switch]$ScanWearableDescriptor
)

$ErrorActionPreference = 'Stop'

if (-not ('NskinzReadOnlyProcessMemory' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class NskinzReadOnlyProcessMemory
{
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern IntPtr OpenProcess(
        UInt32 desiredAccess,
        [MarshalAs(UnmanagedType.Bool)] bool inheritHandle,
        Int32 processId);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool ReadProcessMemory(
        IntPtr process,
        UInt64 address,
        byte[] buffer,
        UIntPtr size,
        out UIntPtr bytesRead);

    [DllImport("kernel32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool CloseHandle(IntPtr handle);

    public static Int32[] FindBytes(byte[] haystack, byte[] needle, Int32 maximumMatches)
    {
        var matches = new System.Collections.Generic.List<Int32>();
        if (haystack == null || needle == null || needle.Length == 0 || maximumMatches <= 0)
            return matches.ToArray();
        for (Int32 offset = 0; offset <= haystack.Length - needle.Length; ++offset)
        {
            bool equal = true;
            for (Int32 index = 0; index < needle.Length; ++index)
            {
                if (haystack[offset + index] != needle[index])
                {
                    equal = false;
                    break;
                }
            }
            if (!equal)
                continue;
            matches.Add(offset);
            if (matches.Count >= maximumMatches)
                break;
        }
        return matches.ToArray();
    }
}
'@
}

if ($ProcessId -eq 0) {
    $process = Get-Process cs2 -ErrorAction Stop | Select-Object -First 1
    $ProcessId = $process.Id
} else {
    $process = Get-Process -Id $ProcessId -ErrorAction Stop
}

$modules = @($process.Modules)
$client = $modules | Where-Object ModuleName -ieq 'client.dll' | Select-Object -First 1
if ($null -eq $client) {
    throw 'client.dll is not loaded in the selected CS2 process.'
}

$processHandle = [NskinzReadOnlyProcessMemory]::OpenProcess(0x410, $false, $ProcessId)
if ($processHandle -eq [IntPtr]::Zero) {
    throw "OpenProcess failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
}

function Read-Bytes([UInt64]$Address, [int]$Length) {
    $buffer = [byte[]]::new($Length)
    [UIntPtr]$read = [UIntPtr]::Zero
    if (-not [NskinzReadOnlyProcessMemory]::ReadProcessMemory(
            $processHandle,
            $Address,
            $buffer,
            [UIntPtr]::new([UInt64]$Length),
            [ref]$read) -or $read.ToUInt64() -ne [UInt64]$Length) {
        return $null
    }
    return $buffer
}

function Read-U64([UInt64]$Address) {
    $buffer = Read-Bytes $Address 8
    if ($null -eq $buffer) { return [UInt64]0 }
    return [BitConverter]::ToUInt64($buffer, 0)
}

function Read-Ascii([UInt64]$Address, [int]$MaximumLength = 512) {
    if ($Address -eq 0) { return '' }
    $buffer = Read-Bytes $Address $MaximumLength
    if ($null -eq $buffer) { return '' }
    $length = [Array]::IndexOf($buffer, [byte]0)
    if ($length -lt 0) { $length = $buffer.Length }
    return [Text.Encoding]::ASCII.GetString($buffer, 0, $length)
}

function Resolve-Module([UInt64]$Address) {
    foreach ($module in $modules) {
        $begin = [UInt64]$module.BaseAddress.ToInt64()
        $end = $begin + [UInt64]$module.ModuleMemorySize
        if ($Address -ge $begin -and $Address -lt $end) {
            return ('{0}+0x{1:X}' -f $module.ModuleName, ($Address - $begin))
        }
    }
    return ''
}

try {
    $clientBase = [UInt64]$client.BaseAddress.ToInt64()
    if ($ScanWearableDescriptor) {
        $moduleBytes = Read-Bytes $clientBase $client.ModuleMemorySize
        if ($null -eq $moduleBytes) {
            throw 'Could not read the complete client.dll image for the descriptor scan.'
        }

        $nameBytes = [Text.Encoding]::ASCII.GetBytes("CEconWearable`0")
        $nameOffsets = [NskinzReadOnlyProcessMemory]::FindBytes($moduleBytes, $nameBytes, 16)
        Write-Output ('pid={0} client=0x{1:X} size=0x{2:X} wearable_names={3}' -f `
            $ProcessId, $clientBase, $client.ModuleMemorySize, $nameOffsets.Count)
        foreach ($nameOffset in $nameOffsets) {
            $nameAddress = $clientBase + [UInt64]$nameOffset
            $pointerBytes = [BitConverter]::GetBytes($nameAddress)
            $referenceOffsets = [NskinzReadOnlyProcessMemory]::FindBytes($moduleBytes, $pointerBytes, 64)
            Write-Output ('name=0x{0:X} rva=0x{1:X} references={2}' -f `
                $nameAddress, $nameOffset, $referenceOffsets.Count)
            foreach ($referenceOffset in $referenceOffsets) {
                $referenceAddress = $clientBase + [UInt64]$referenceOffset
                Write-Output ('  reference=0x{0:X} rva=0x{1:X}' -f $referenceAddress, $referenceOffset)
                for ($relative = -0x30; $relative -le 0x50; $relative += 8) {
                    if (($referenceOffset + $relative) -lt 0 -or
                        ($referenceOffset + $relative + 8) -gt $moduleBytes.Length) {
                        continue
                    }
                    $value = [BitConverter]::ToUInt64($moduleBytes, $referenceOffset + $relative)
                    $fieldAddress = if ($relative -lt 0) {
                        $referenceAddress - [UInt64](-$relative)
                    } else {
                        $referenceAddress + [UInt64]$relative
                    }
                    $relativeLabel = if ($relative -lt 0) {
                        ('-0x{0:X}' -f (-$relative))
                    } elseif ($relative -gt 0) {
                        ('+0x{0:X}' -f $relative)
                    } else {
                        '0'
                    }
                    $ascii = Read-Ascii $value 160
                    if ($ascii -notmatch '^[ -~]{3,}$') { $ascii = '' }
                    [pscustomobject]@{
                        Relative = $relativeLabel
                        Address = ('0x{0:X}' -f $fieldAddress)
                        Value = ('0x{0:X}' -f $value)
                        Module = Resolve-Module $value
                        Ascii = $ascii
                    }
                }
            }
        }
        return
    }

    if ($DumpRva -ne 0 -or $DumpAddress -ne 0) {
        $dumpAddress = if ($DumpAddress -ne 0) { $DumpAddress } else { $clientBase + $DumpRva }
        Write-Output ('pid={0} client=0x{1:X} dump=0x{2:X} compare=0x{3:X}' -f $ProcessId, $clientBase, $dumpAddress, $CompareAddress)
        for ($offset = 0; $offset -lt $DumpLength; $offset += 8) {
            $value = Read-U64 ($dumpAddress + [UInt64]$offset)
            $compareValue = if ($CompareAddress -ne 0) {
                Read-U64 ($CompareAddress + [UInt64]$offset)
            } else {
                [UInt64]0
            }
            if ($CompareAddress -ne 0 -and $value -eq $compareValue) {
                continue
            }
            $ascii = Read-Ascii $value 160
            if ($ascii -notmatch '^[ -~]{3,}$') { $ascii = '' }
            [pscustomobject]@{
                Offset = ('0x{0:X}' -f $offset)
                Value = ('0x{0:X}' -f $value)
                Module = Resolve-Module $value
                CompareValue = if ($CompareAddress -ne 0) { ('0x{0:X}' -f $compareValue) } else { '' }
                CompareModule = if ($CompareAddress -ne 0) { Resolve-Module $compareValue } else { '' }
                Ascii = $ascii
            }
        }
        return
    }

    $recordCount = [int](($TableEndRva - $TableBeginRva) / [UInt64]$RecordSize)
    Write-Output ('pid={0} client=0x{1:X} records={2}' -f $ProcessId, $clientBase, $recordCount)

    for ($index = 0; $index -lt $recordCount; ++$index) {
        $recordAddress = $clientBase + $TableBeginRva + [UInt64]($index * $RecordSize)
        $pathPointer = Read-U64 $recordAddress
        $path = Read-Ascii $pathPointer
        if ([string]::IsNullOrWhiteSpace($path) -or
            ($path -notmatch 'glove|arms|sleeve')) {
            continue
        }

        $field08 = Read-U64 ($recordAddress + 0x08)
        $field10 = Read-U64 ($recordAddress + 0x10)
        $field18 = Read-U64 ($recordAddress + 0x18)
        $field20 = Read-U64 ($recordAddress + 0x20)
        $linkedPaths = @()
        if ($ExpandField18 -and $field18 -ne 0) {
            for ($linkedIndex = 0; $linkedIndex -lt 5; ++$linkedIndex) {
                $linkedPathPointer = Read-U64 ($field18 + [UInt64]($linkedIndex * 0x28))
                $linkedPath = Read-Ascii $linkedPathPointer
                if (-not [string]::IsNullOrWhiteSpace($linkedPath)) {
                    $linkedPaths += $linkedPath
                }
            }
        }

        [pscustomobject]@{
            Index = $index
            Record = ('0x{0:X}' -f $recordAddress)
            Path = $path
            Field08 = ('0x{0:X}' -f $field08)
            Field10 = ('0x{0:X}' -f $field10)
            Field10Module = Resolve-Module $field10
            Field18 = ('0x{0:X}' -f $field18)
            Field18Module = Resolve-Module $field18
            Field20 = ('0x{0:X}' -f $field20)
            LinkedPaths = $linkedPaths -join '; '
        }
    }
} finally {
    [void][NskinzReadOnlyProcessMemory]::CloseHandle($processHandle)
}
