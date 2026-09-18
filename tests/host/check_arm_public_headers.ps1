<#
.SYNOPSIS
Strict-compiles the public-header C and C++ translation units for 32-bit Arm.

.DESCRIPTION
Checks Cortex-M85 and Cortex-M55 with warnings as errors and without producing
object files. The C++ translation unit includes the 32-bit Sentinel ABI
assertions.

.EXAMPLE
pwsh tests/host/check_arm_public_headers.ps1 -ToolchainBin C:\path\to\arm-none-eabi\bin
#>

param(
    [Parameter(Mandatory = $true)]
    [string] $ToolchainBin
)

$ErrorActionPreference = 'Stop'

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$compilerSuffix = if ($IsWindows -or $env:OS -eq 'Windows_NT') { '.exe' } else { '' }
$cCompiler = Join-Path $ToolchainBin "arm-none-eabi-gcc$compilerSuffix"
$cxxCompiler = Join-Path $ToolchainBin "arm-none-eabi-g++$compilerSuffix"

foreach ($compiler in @($cCompiler, $cxxCompiler)) {
    if (-not (Test-Path -LiteralPath $compiler -PathType Leaf)) {
        throw "Arm compiler not found: $compiler"
    }
}

$includeArguments = @(
    '-I', (Join-Path $repositoryRoot 'src'),
    '-I', (Join-Path $repositoryRoot 'src\block'),
    '-I', (Join-Path $repositoryRoot 'src\sentinel'),
    '-I', (Join-Path $repositoryRoot 'apps\sentinel-lab\application')
)
$definitionArguments = @(
    '-DMTFS_ENABLE_SEALED_MODEL=1',
    '-DMTFS_ENABLE_STORAGE_SENTINEL=1',
    '-DMTFS_ENABLE_STORAGE_SENTINEL_INFERENCE=1'
)
$warningArguments = @('-Wall', '-Wextra', '-Wpedantic', '-Werror', '-fsyntax-only')
$cSource = Join-Path $PSScriptRoot 'test_public_headers.c'
$cxxSource = Join-Path $PSScriptRoot 'test_public_headers.cpp'

foreach ($cpu in @('cortex-m85', 'cortex-m55')) {
    Write-Host "Checking public C headers for $cpu"
    & $cCompiler '-std=c11' "-mcpu=$cpu" '-mthumb' @warningArguments `
        @definitionArguments @includeArguments $cSource
    if ($LASTEXITCODE -ne 0) {
        throw "Public C header check failed for $cpu"
    }

    Write-Host "Checking public C++ headers and 32-bit ABI assertions for $cpu"
    & $cxxCompiler '-std=c++11' "-mcpu=$cpu" '-mthumb' @warningArguments `
        @definitionArguments @includeArguments $cxxSource
    if ($LASTEXITCODE -ne 0) {
        throw "Public C++ header check failed for $cpu"
    }
}
