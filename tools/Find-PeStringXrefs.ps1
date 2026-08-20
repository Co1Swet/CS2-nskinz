param(
    [Parameter(Mandatory = $true)]
    [string]$Path,

    [Parameter(Mandatory = $true)]
    [string]$Text
)

$bytes = [System.IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $Path))
$peOffset = [BitConverter]::ToInt32($bytes, 0x3c)
if ([BitConverter]::ToUInt32($bytes, $peOffset) -ne 0x00004550) {
    throw "Not a PE image: $Path"
}

$coffOffset = $peOffset + 4
$sectionCount = [BitConverter]::ToUInt16($bytes, $coffOffset + 2)
$optionalSize = [BitConverter]::ToUInt16($bytes, $coffOffset + 16)
$optionalOffset = $coffOffset + 20
$magic = [BitConverter]::ToUInt16($bytes, $optionalOffset)
if ($magic -ne 0x20b) {
    throw "Expected a PE32+ image: $Path"
}

$imageBase = [BitConverter]::ToUInt64($bytes, $optionalOffset + 24)
$sectionOffset = $optionalOffset + $optionalSize
$sections = @()
for ($index = 0; $index -lt $sectionCount; ++$index) {
    $header = $sectionOffset + 40 * $index
    $nameBytes = $bytes[$header..($header + 7)]
    $name = [Text.Encoding]::ASCII.GetString($nameBytes).Trim([char]0)
    $sections += [pscustomobject]@{
        Name = $name
        VirtualSize = [BitConverter]::ToUInt32($bytes, $header + 8)
        VirtualAddress = [BitConverter]::ToUInt32($bytes, $header + 12)
        RawSize = [BitConverter]::ToUInt32($bytes, $header + 16)
        RawOffset = [BitConverter]::ToUInt32($bytes, $header + 20)
    }
}

function Find-Bytes([byte[]]$Haystack, [byte[]]$Needle) {
    for ($offset = 0; $offset -le $Haystack.Length - $Needle.Length; ++$offset) {
        $matches = $true
        for ($index = 0; $index -lt $Needle.Length; ++$index) {
            if ($Haystack[$offset + $index] -ne $Needle[$index]) {
                $matches = $false
                break
            }
        }
        if ($matches) { $offset }
    }
}

function Convert-RawOffsetToRva([uint32]$RawOffset) {
    foreach ($section in $sections) {
        if ($RawOffset -ge $section.RawOffset -and $RawOffset -lt $section.RawOffset + $section.RawSize) {
            return [uint32]($section.VirtualAddress + $RawOffset - $section.RawOffset)
        }
    }
    throw "Raw offset 0x$($RawOffset.ToString('x')) is not mapped by a section"
}

$needle = [Text.Encoding]::Unicode.GetBytes($Text + [char]0)
$stringOffsets = @(Find-Bytes $bytes $needle)
if ($stringOffsets.Count -ne 1) {
    throw "Expected one UTF-16 string '$Text', found $($stringOffsets.Count)"
}

$stringRva = Convert-RawOffsetToRva $stringOffsets[0]
$textSection = $sections | Where-Object Name -eq '.text' | Select-Object -First 1
if (-not $textSection) { throw 'PE image has no .text section' }

$xrefs = @()
$textEnd = $textSection.RawOffset + $textSection.RawSize
for ($raw = $textSection.RawOffset; $raw -le $textEnd - 4; ++$raw) {
    $displacement = [BitConverter]::ToInt32($bytes, $raw)
    $displacementRva = [int64]$textSection.VirtualAddress + ($raw - $textSection.RawOffset)
    if ($displacementRva + 4 + $displacement -eq $stringRva) {
        $instructionRaw = [Math]::Max([int64]$textSection.RawOffset, $raw - 4)
        $instructionRva = [int64]$textSection.VirtualAddress + ($instructionRaw - $textSection.RawOffset)
        $contextStart = [Math]::Max([int64]$textSection.RawOffset, $raw - 12)
        $contextLength = [Math]::Min(32, $textEnd - $contextStart)
        $context = ($bytes[$contextStart..($contextStart + $contextLength - 1)] | ForEach-Object { $_.ToString('x2') }) -join ' '
        $xrefs += [pscustomobject]@{
            DisplacementRawOffset = ('0x{0:x}' -f $raw)
            CandidateInstructionRva = ('0x{0:x}' -f $instructionRva)
            CandidateInstructionVa = ('0x{0:x}' -f ($imageBase + $instructionRva))
            Context = $context
        }
    }
}

[pscustomobject]@{
    ImageBase = ('0x{0:x}' -f $imageBase)
    StringRawOffset = ('0x{0:x}' -f $stringOffsets[0])
    StringRva = ('0x{0:x}' -f $stringRva)
    StringVa = ('0x{0:x}' -f ($imageBase + $stringRva))
    Xrefs = $xrefs
} | ConvertTo-Json -Depth 4
