param(
    [Parameter(Mandatory = $true)]
    [string]$Path,

    [Parameter(Mandatory = $true)]
    [string[]]$Pattern
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
$imageBase = [BitConverter]::ToUInt64($bytes, $optionalOffset + 24)
$sectionOffset = $optionalOffset + $optionalSize
$sections = @()
for ($index = 0; $index -lt $sectionCount; ++$index) {
    $header = $sectionOffset + 40 * $index
    $name = [Text.Encoding]::ASCII.GetString($bytes[$header..($header + 7)]).Trim([char]0)
    $sections += [pscustomobject]@{
        Name = $name
        VirtualAddress = [BitConverter]::ToUInt32($bytes, $header + 12)
        RawSize = [BitConverter]::ToUInt32($bytes, $header + 16)
        RawOffset = [BitConverter]::ToUInt32($bytes, $header + 20)
    }
}

$text = $sections | Where-Object Name -eq '.text' | Select-Object -First 1
if (-not $text) {
    throw 'PE image has no .text section'
}

$results = @()
foreach ($idaPattern in $Pattern) {
    $tokens = @($idaPattern -split '\s+' | Where-Object { $_ })
    $compiled = @($tokens | ForEach-Object {
        if ($_ -eq '?' -or $_ -eq '??') { -1 } else { [Convert]::ToInt32($_, 16) }
    })
    $anchorIndex = 0
    while ($anchorIndex -lt $compiled.Count -and $compiled[$anchorIndex] -lt 0) {
        ++$anchorIndex
    }
    if ($anchorIndex -ge $compiled.Count) {
        throw "Pattern contains no concrete byte: $idaPattern"
    }

    $matches = @()
    $begin = [int]$text.RawOffset
    $end = [int]($text.RawOffset + $text.RawSize - $compiled.Count)
    for ($raw = $begin; $raw -le $end; ++$raw) {
        if ($bytes[$raw + $anchorIndex] -ne $compiled[$anchorIndex]) {
            continue
        }
        $matched = $true
        for ($byteIndex = 0; $byteIndex -lt $compiled.Count; ++$byteIndex) {
            if ($compiled[$byteIndex] -ge 0 -and $bytes[$raw + $byteIndex] -ne $compiled[$byteIndex]) {
                $matched = $false
                break
            }
        }
        if ($matched) {
            $rva = [uint64]$text.VirtualAddress + [uint64]($raw - $text.RawOffset)
            $matches += [pscustomobject]@{
                RawOffset = ('0x{0:X}' -f $raw)
                Rva = ('0x{0:X}' -f $rva)
                Va = ('0x{0:X}' -f ($imageBase + $rva))
            }
        }
    }

    $results += [pscustomobject]@{
        Pattern = $idaPattern
        MatchCount = $matches.Count
        Matches = $matches
    }
}

$results | ConvertTo-Json -Depth 5
