param(
    [string]$Source = (Join-Path $PSScriptRoot '..\hlae_2_191_1\x64\AfxHookSource2.dll'),
    [string]$Output = (Join-Path $PSScriptRoot '..\hlae_2_191_1\x64\AfxHookSource2.nskinz-test.dll')
)

# HLAE 2.191.1's early rendersystemdx11 import hook currently makes the
# installed CS2 build abort during NVAPI initialization. nSkinz does not need
# that render hook, but it does need HLAE to be present before engine2/client.
# Patch a separately named, hash-pinned test copy only; never touch the bundled
# HLAE DLL in place.
$expectedSha256 = 'ADCE361D0FA994D2E04627E70DB8D8DDC6E8B3409237384FD7584A157EF0BD49'
$patchOffset = 0x36aba3
$expected = [byte[]](0xe9, 0x88, 0x09, 0x01, 0x00)
$replacement = [byte[]](0xc3, 0x90, 0x90, 0x90, 0x90)

$resolvedSource = (Resolve-Path -LiteralPath $Source).Path
$actualSha256 = (Get-FileHash -LiteralPath $resolvedSource -Algorithm SHA256).Hash
if ($actualSha256 -ne $expectedSha256) {
    throw "Refusing to patch an unknown AfxHookSource2.dll. Expected SHA-256 $expectedSha256, got $actualSha256"
}

$bytes = [System.IO.File]::ReadAllBytes($resolvedSource)
for ($index = 0; $index -lt $expected.Length; ++$index) {
    if ($bytes[$patchOffset + $index] -ne $expected[$index]) {
        $found = ($bytes[$patchOffset..($patchOffset + $expected.Length - 1)] | ForEach-Object { $_.ToString('X2') }) -join ' '
        throw "Unexpected instruction bytes at file offset 0x$($patchOffset.ToString('X')): $found"
    }
}

for ($index = 0; $index -lt $replacement.Length; ++$index) {
    $bytes[$patchOffset + $index] = $replacement[$index]
}

$outputDirectory = Split-Path -Parent $Output
if (-not (Test-Path -LiteralPath $outputDirectory)) {
    New-Item -ItemType Directory -Path $outputDirectory | Out-Null
}
[System.IO.File]::WriteAllBytes($Output, $bytes)

$outputItem = Get-Item -LiteralPath $Output
[pscustomobject]@{
    Source = $resolvedSource
    SourceSha256 = $actualSha256
    Output = $outputItem.FullName
    OutputSha256 = (Get-FileHash -LiteralPath $outputItem.FullName -Algorithm SHA256).Hash
    PatchedFileOffset = ('0x{0:X}' -f $patchOffset)
    OriginalInstruction = 'jmp Hook_RenderSystemDX11'
    ReplacementInstruction = 'ret; nop; nop; nop; nop'
}
