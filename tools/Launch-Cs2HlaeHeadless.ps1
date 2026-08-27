param(
    [Parameter(Mandatory = $true)]
    [string]$HlaePath,

    [Parameter(Mandatory = $true)]
    [string]$Cs2Path,

    [Parameter(Mandatory = $true)]
    [string]$MovieMakingConfigDirectory,

    [int]$NetConPort = 2121,
    [int]$Width = 1440,
    [int]$Height = 900
)

$ErrorActionPreference = 'Stop'

$resolvedHlaePath = (Resolve-Path -LiteralPath $HlaePath).Path
$resolvedCs2Path = (Resolve-Path -LiteralPath $Cs2Path).Path
$resolvedMovieMakingConfigDirectory =
    (Resolve-Path -LiteralPath $MovieMakingConfigDirectory).Path

$assembly = [Reflection.Assembly]::LoadFrom($resolvedHlaePath)
$configType = $assembly.GetType('AfxGui.CfgLauncherCs2', $true)
$launcherType = $assembly.GetType('AfxGui.LaunchCs2', $true)
$config = [Activator]::CreateInstance($configType)
$config.Cs2Exe = $resolvedCs2Path
$config.MmcfgEnabled = $true
$config.Mmcfg = $resolvedMovieMakingConfigDirectory
$config.GfxEnabled = $true
$config.GfxWidth = [uint16]$Width
$config.GfxHeight = [uint16]$Height
$config.GfxFull = $false
$config.AvoidVac = $true
$config.CustomLaunchOptions =
    "+sv_lan 1 -console -novid -afxFixNetCon -netconport $NetConPort"
$config.RememberChanges = $false

$launchMethod = $launcherType.GetMethod(
    'Launch',
    [Reflection.BindingFlags]'Static,Public')
if ($null -eq $launchMethod) {
    throw 'HLAE LaunchCs2.Launch could not be resolved.'
}
$launchAccepted = $launchMethod.Invoke($null, @($config))
if (-not $launchAccepted) {
    throw 'HLAE LaunchCs2 returned failure.'
}

Write-Output 'HLAE LaunchCs2 accepted the isolated background demo configuration.'
