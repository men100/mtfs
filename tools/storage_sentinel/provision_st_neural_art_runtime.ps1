param(
    [Parameter(Mandatory = $false)]
    [string]$STEdgeAIRoot = 'C:\ST\STEdgeAI\4.0'
)

$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$source = Join-Path $STEdgeAIRoot 'Middlewares\ST\AI\Npu'
$stProjectDrivers = Join-Path $STEdgeAIRoot 'Projects\STM32N6570-DK\Applications\Drivers\STM32N6xx_HAL_Driver'
$destination = Join-Path $repository 'external\stm32_cube\stm32n6570_dk\Middlewares\ST\AI\Npu'
$versionTool = Join-Path $STEdgeAIRoot 'Utilities\windows\stedgeai.exe'
$atonnTool = Join-Path $STEdgeAIRoot 'Utilities\windows\atonn.exe'
$requiredSources = @(
    'ecloader.c',
    'll_aton.c',
    'll_aton_cipher.c',
    'll_aton_dbgtrc.c',
    'll_aton_debug.c',
    'll_aton_lib.c',
    'll_aton_lib_sw_operators.c',
    'll_aton_reloc_network.c',
    'll_aton_runtime.c',
    'll_aton_util.c',
    'll_sw_float.c',
    'll_sw_integer.c'
)

if (-not (Test-Path -LiteralPath $versionTool -PathType Leaf) -or
    -not (Test-Path -LiteralPath $atonnTool -PathType Leaf)) {
    throw "ST Edge AI Core executables were not found under: $STEdgeAIRoot"
}
$version = & $versionTool --version 2>&1 | Out-String
$versionExit = $LASTEXITCODE
$atonnVersion = & $atonnTool --version 2>&1 | Out-String
$atonnExit = $LASTEXITCODE
if ($versionExit -ne 0 -or $atonnExit -ne 0 -or
    $version -notmatch 'ST Edge AI Core v4\.0\.1-' -or
    $atonnVersion -notmatch '(Version atonn\.|atonn-v)1\.1\.3') {
    throw 'This integration requires ST Edge AI Core 4.0.1 with atonn 1.1.3.'
}
if (-not (Test-Path -LiteralPath (Join-Path $source 'll_aton\ll_aton_runtime.c'))) {
    throw "Neural-ART runtime sources were not found under: $source"
}

New-Item -ItemType Directory -Force -Path (Join-Path $destination 'll_aton') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $destination 'Devices\STM32N6xx') | Out-Null
Get-ChildItem -LiteralPath (Join-Path $source 'll_aton') -Filter '*.h' -File |
    Copy-Item -Destination (Join-Path $destination 'll_aton') -Force
foreach ($name in $requiredSources) {
    Copy-Item -LiteralPath (Join-Path $source "ll_aton\$name") -Destination (Join-Path $destination 'll_aton') -Force
}
Get-ChildItem -LiteralPath (Join-Path $source 'Devices\STM32N6xx') -Filter '*.h' -File |
    Copy-Item -Destination (Join-Path $destination 'Devices\STM32N6xx') -Force
Copy-Item -LiteralPath (Join-Path $source 'Devices\STM32N6xx\mcu_cache.c') -Destination (Join-Path $destination 'Devices\STM32N6xx') -Force
Copy-Item -LiteralPath (Join-Path $source 'Devices\STM32N6xx\npu_cache.c') -Destination (Join-Path $destination 'Devices\STM32N6xx') -Force
Copy-Item -LiteralPath (Join-Path $stProjectDrivers 'Inc\stm32n6xx_hal_rif.h') -Destination (Join-Path $destination 'Devices\STM32N6xx') -Force
Copy-Item -LiteralPath (Join-Path $stProjectDrivers 'Src\stm32n6xx_hal_rif.c') -Destination (Join-Path $destination 'Devices\STM32N6xx') -Force
Copy-Item -LiteralPath (Join-Path $stProjectDrivers 'Inc\stm32n6xx_hal_cacheaxi.h') -Destination (Join-Path $destination 'Devices\STM32N6xx') -Force
Copy-Item -LiteralPath (Join-Path $stProjectDrivers 'Src\stm32n6xx_hal_cacheaxi.c') -Destination (Join-Path $destination 'Devices\STM32N6xx') -Force
Copy-Item -LiteralPath (Join-Path $source '..\LICENSE.txt') -Destination $destination -Force

Write-Host 'Provisioned ST Neural-ART runtime 4.0.1 (atonn 1.1.3).'
Write-Host $destination
