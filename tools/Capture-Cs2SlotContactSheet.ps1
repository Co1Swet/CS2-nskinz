param(
    [Parameter(Mandatory = $true)]
    [int]$TargetProcessId,

    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,

    [int]$Port = 2121
)

$ErrorActionPreference = 'Stop'
$resolvedOutput = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $resolvedOutput -Force | Out-Null

foreach ($slot in 0..9) {
    & "$PSScriptRoot\Invoke-Cs2NetCon.ps1" `
        -Command "spec_player $slot; spec_mode 5; demo_resume" `
        -Port $Port `
        -WaitMilliseconds 80 | Out-Null
    Start-Sleep -Milliseconds 350
    & "$PSScriptRoot\Invoke-Cs2NetCon.ps1" `
        -Command 'demo_pause' `
        -Port $Port `
        -WaitMilliseconds 80 | Out-Null
    & "$PSScriptRoot\Capture-Cs2Window.ps1" `
        -TargetProcessId $TargetProcessId `
        -OutputPath (Join-Path $resolvedOutput "slot_$slot.png") | Out-Null
}

Add-Type -AssemblyName System.Drawing
$sheet = [Drawing.Bitmap]::new(720, 1125)
$graphics = [Drawing.Graphics]::FromImage($sheet)
$font = [Drawing.Font]::new('Arial', 18, [Drawing.FontStyle]::Bold)
try {
    $graphics.Clear([Drawing.Color]::Black)
    foreach ($slot in 0..9) {
        $image = [Drawing.Image]::FromFile((Join-Path $resolvedOutput "slot_$slot.png"))
        try {
            $column = $slot % 2
            $row = [Math]::Floor($slot / 2)
            $x = $column * 360
            $y = $row * 225
            $graphics.DrawImage($image, $x, $y, 360, 225)
            $graphics.FillRectangle([Drawing.Brushes]::Black, $x, $y, 86, 31)
            $graphics.DrawString("slot $slot", $font, [Drawing.Brushes]::Yellow, $x + 4, $y + 3)
        }
        finally {
            $image.Dispose()
        }
    }

    $contactSheetPath = Join-Path $resolvedOutput 'contact_sheet.png'
    $sheet.Save($contactSheetPath, [Drawing.Imaging.ImageFormat]::Png)
    Get-Item -LiteralPath $contactSheetPath | Select-Object FullName, Length, LastWriteTime
}
finally {
    $font.Dispose()
    $graphics.Dispose()
    $sheet.Dispose()
}
