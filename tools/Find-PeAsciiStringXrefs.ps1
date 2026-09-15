param(
    [Parameter(Mandatory = $true)]
    [string]$Path,

    [Parameter(Mandatory = $true)]
    [string]$Text,

    [int]$MaxResults = 32
)

$resolvedPath = (Resolve-Path -LiteralPath $Path).Path
$bytes = [System.IO.File]::ReadAllBytes($resolvedPath)
$peOffset = [BitConverter]::ToInt32($bytes, 0x3c)
if ([BitConverter]::ToUInt32($bytes, $peOffset) -ne 0x00004550) {
    throw "Not a PE image: $resolvedPath"
}

$coffOffset = $peOffset + 4
$sectionCount = [BitConverter]::ToUInt16($bytes, $coffOffset + 2)
$optionalSize = [BitConverter]::ToUInt16($bytes, $coffOffset + 16)
$optionalOffset = $coffOffset + 20
if ([BitConverter]::ToUInt16($bytes, $optionalOffset) -ne 0x20b) {
    throw "Expected a PE32+ image: $resolvedPath"
}

$imageBase = [BitConverter]::ToUInt64($bytes, $optionalOffset + 24)
$sectionOffset = $optionalOffset + $optionalSize
$sections = @()
for ($index = 0; $index -lt $sectionCount; ++$index) {
    $header = $sectionOffset + 40 * $index
    $sections += [pscustomobject]@{
        Name = [Text.Encoding]::ASCII.GetString($bytes[$header..($header + 7)]).Trim([char]0)
        VirtualAddress = [BitConverter]::ToUInt32($bytes, $header + 12)
        RawSize = [BitConverter]::ToUInt32($bytes, $header + 16)
        RawOffset = [BitConverter]::ToUInt32($bytes, $header + 20)
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

$needle = [Text.Encoding]::ASCII.GetBytes($Text + [char]0)
$stringOffsets = [System.Collections.Generic.List[int]]::new()
for ($raw = 0; $raw -le $bytes.Length - $needle.Length; ++$raw) {
    if ($bytes[$raw] -ne $needle[0]) { continue }
    $matched = $true
    for ($index = 1; $index -lt $needle.Length; ++$index) {
        if ($bytes[$raw + $index] -ne $needle[$index]) {
            $matched = $false
            break
        }
    }
    if ($matched) { $stringOffsets.Add($raw) }
}

$textSection = $sections | Where-Object Name -eq '.text' | Select-Object -First 1
if (-not $textSection) { throw 'PE image has no .text section' }
$textEnd = [int]($textSection.RawOffset + $textSection.RawSize)

$strings = @()
foreach ($stringRawOffset in $stringOffsets) {
    $stringRva = Convert-RawOffsetToRva $stringRawOffset
    $xrefs = [System.Collections.Generic.List[object]]::new()
    for ($raw = [int]$textSection.RawOffset; $raw -le $textEnd - 4; ++$raw) {
        $displacement = [BitConverter]::ToInt32($bytes, $raw)
        $displacementRva = [int64]$textSection.VirtualAddress + ($raw - $textSection.RawOffset)
        if ($displacementRva + 4 + $displacement -ne $stringRva) { continue }

        $contextStart = [Math]::Max([int]$textSection.RawOffset, $raw - 12)
        $contextEnd = [Math]::Min($textEnd - 1, $raw + 19)
        $context = ($bytes[$contextStart..$contextEnd] | ForEach-Object { $_.ToString('x2') }) -join ' '
        $xrefs.Add([pscustomobject]@{
            DisplacementRva = ('0x{0:X}' -f $displacementRva)
            CandidateInstructionRva = ('0x{0:X}' -f ($displacementRva - 4))
            Context = $context
        })
        if ($xrefs.Count -ge $MaxResults) { break }
    }

    $strings += [pscustomobject]@{
        StringRawOffset = ('0x{0:X}' -f $stringRawOffset)
        StringRva = ('0x{0:X}' -f $stringRva)
        StringVa = ('0x{0:X}' -f ($imageBase + $stringRva))
        XrefCountReturned = $xrefs.Count
        Xrefs = $xrefs
    }
}

[pscustomobject]@{
    ImageBase = ('0x{0:X}' -f $imageBase)
    MatchCount = $strings.Count
    Strings = $strings
} | ConvertTo-Json -Depth 5
