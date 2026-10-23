# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.

[CmdletBinding()]
param(
    [string]$DxcSource = '',
    [string]$BuildRoot = '',
    [switch]$CleanBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $PSScriptRoot
$VendorRoot = Join-Path $RepoRoot 'tools\shader_compiler\windows'
$VendorBin = Join-Path $VendorRoot 'bin'
$DxcNotices = Join-Path $RepoRoot 'tools\shader_compiler\licenses\dxc'
$BuildInfo = Join-Path $VendorRoot 'DXC_BUILDINFO.txt'

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed ($LASTEXITCODE): $FilePath $($Arguments -join ' ')"
    }
}

function Require-Command {
    param([Parameter(Mandatory = $true)][string]$Name)

    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if (-not $cmd) {
        throw "Required command not found: $Name"
    }
    return $cmd.Source
}

function Import-VsDevEnvironment {
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
        return
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw 'cl.exe is not available and vswhere.exe was not found. Visual Studio C++ build tools are required.'
    }

    $installPath = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
    if (-not $installPath) {
        throw 'No Visual Studio installation with the x64 C++ toolchain was found.'
    }

    $vsDevCmd = Join-Path $installPath 'Common7\Tools\VsDevCmd.bat'
    if (-not (Test-Path -LiteralPath $vsDevCmd -PathType Leaf)) {
        throw "VsDevCmd.bat was not found at: $vsDevCmd"
    }

    $lines = & $env:ComSpec /s /c "`"$vsDevCmd`" -no_logo -arch=amd64 -host_arch=amd64 >nul && set"
    if ($LASTEXITCODE -ne 0) {
        throw 'VsDevCmd.bat failed to initialize the x64 build environment.'
    }

    foreach ($line in $lines) {
        if ($line -match '^([^=]+)=(.*)$') {
            [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
        }
    }

    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        throw 'Visual Studio environment was imported, but cl.exe is still unavailable.'
    }
}

function Get-GitText {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)

    $text = (& git @Arguments)
    if ($LASTEXITCODE -ne 0) {
        throw "git failed: git $($Arguments -join ' ')"
    }
    return (($text | Out-String).Trim())
}

function Get-FileSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

function Resolve-BuiltFile {
    param([Parameter(Mandatory = $true)][string]$Name)

    $candidates = @(
        (Join-Path $BuildRoot "bin\$Name"),
        (Join-Path $BuildRoot "bin\Release\$Name"),
        (Join-Path $BuildRoot "Release\bin\$Name")
    )

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }

    throw "Expected DXC build product was not produced: $Name"
}

if ($env:OS -ne 'Windows_NT') {
    throw 'This refresh script must run on Windows.'
}
if (-not [Environment]::Is64BitOperatingSystem) {
    throw 'APUSR vendors Windows x64 DXC only.'
}

if (-not $DxcSource) {
    if ($env:DXC_SOURCE_DIR) {
        $DxcSource = $env:DXC_SOURCE_DIR
    } else {
        $siblingCheckout = Join-Path (Split-Path -Parent $RepoRoot) 'DirectXShaderCompiler'
        if (Test-Path -LiteralPath (Join-Path $siblingCheckout '.git')) {
            $DxcSource = $siblingCheckout
        } else {
            throw 'DXC source was not specified. Pass -DxcSource <path>, set DXC_SOURCE_DIR, or place a DirectXShaderCompiler checkout beside the APUSR repository.'
        }
    }
}

$DxcSource = [IO.Path]::GetFullPath($DxcSource)
if (-not (Test-Path -LiteralPath (Join-Path $DxcSource '.git'))) {
    throw "DXC source is not a git checkout: $DxcSource"
}
if (-not (Test-Path -LiteralPath (Join-Path $DxcSource 'CMakeLists.txt') -PathType Leaf)) {
    throw "DXC CMakeLists.txt not found under: $DxcSource"
}
if (-not $BuildRoot) {
    $BuildRoot = Join-Path $DxcSource 'build-apusr-release'
}
$BuildRoot = [IO.Path]::GetFullPath($BuildRoot)

$null = Require-Command git
$null = Require-Command cmake
$null = Require-Command python
Import-VsDevEnvironment
$null = Require-Command ninja

# We deliberately do not require the WDK here. Upstream's complete Windows
# build/test environment uses it for Windows test infrastructure, but APUSR only
# builds dxc, dxcompiler and dxildll with all LLVM/HLSL/SPIR-V test trees disabled.
# If one of those real targets actually needs an unavailable kit component,
# configuration or compilation below is the authoritative failure.
if ($env:WindowsSDKVersion) {
    $sdkText = $env:WindowsSDKVersion.TrimEnd('\')
    [Version]$sdkVersion = $null
    if ([Version]::TryParse($sdkText, [ref]$sdkVersion) -and $sdkVersion -lt [Version]'10.0.26100.0') {
        throw "Active Windows SDK $sdkVersion is older than DXC's documented minimum 10.0.26100.0."
    }
    Write-Host "[APUSR DXC] Active Windows SDK: $sdkText"
}

Write-Host "[APUSR DXC] Source: $DxcSource"
Write-Host '[APUSR DXC] Ensuring recursive submodules match the checkout'
Invoke-Checked -FilePath 'git' -Arguments @('-C', $DxcSource, 'submodule', 'sync', '--recursive')
Invoke-Checked -FilePath 'git' -Arguments @('-C', $DxcSource, 'submodule', 'update', '--init', '--recursive')

$actualCommit = Get-GitText @('-C', $DxcSource, 'rev-parse', 'HEAD')
$commitDate = Get-GitText @('-C', $DxcSource, 'show', '-s', '--format=%cI', 'HEAD')
$dirtyText = Get-GitText @('-C', $DxcSource, 'status', '--porcelain=v1', '--untracked-files=no')
if ($dirtyText) {
    throw "DXC source checkout has tracked modifications. Refusing a non-reproducible vendor build:`n$dirtyText"
}

$submoduleStatus = Get-GitText @('-C', $DxcSource, 'submodule', 'status', '--recursive')
$badSubmodules = @($submoduleStatus -split "`r?`n" | Where-Object { $_ -match '^[+-U]' })
if ($badSubmodules.Count -ne 0) {
    throw "DXC submodules do not exactly match the recorded revisions:`n$($badSubmodules -join "`n")"
}

Write-Host "[APUSR DXC] Building source commit: $actualCommit"

if ($CleanBuild -and (Test-Path -LiteralPath $BuildRoot)) {
    Write-Host "[APUSR DXC] Removing previous build tree: $BuildRoot"
    Remove-Item -LiteralPath $BuildRoot -Recurse -Force
}

# This is a dedicated APUSR DXC build directory. If a stale cache used another
# generator, discard it rather than forcing the user to diagnose CMake cache state.
$cachePath = Join-Path $BuildRoot 'CMakeCache.txt'
if (Test-Path -LiteralPath $cachePath -PathType Leaf) {
    $cachedGenerator = Select-String -LiteralPath $cachePath -Pattern '^CMAKE_GENERATOR:INTERNAL=(.+)$' | Select-Object -First 1
    if ($cachedGenerator -and $cachedGenerator.Matches[0].Groups[1].Value -ne 'Ninja') {
        Write-Host '[APUSR DXC] Previous APUSR DXC build used another generator; recreating it.'
        Remove-Item -LiteralPath $BuildRoot -Recurse -Force
    }
}

Write-Host '[APUSR DXC] Configuring Release DXC with SPIR-V enabled; tests disabled'
$configureArgs = @(
    '-S', $DxcSource,
    '-B', $BuildRoot,
    '-G', 'Ninja',
    '-DCMAKE_BUILD_TYPE=Release',
    '-DHLSL_INCLUDE_TESTS=OFF',
    '-DLLVM_INCLUDE_TESTS=OFF',
    '-DLLVM_BUILD_TESTS=OFF',
    '-DCLANG_INCLUDE_TESTS=OFF',
    '-DSPIRV_BUILD_TESTS=OFF',
    '-C', (Join-Path $DxcSource 'cmake\caches\PredefinedParams.cmake')
)
Invoke-Checked -FilePath 'cmake' -Arguments $configureArgs

Write-Host '[APUSR DXC] Building dxc.exe + dxcompiler.dll + dxil.dll'
Invoke-Checked -FilePath 'cmake' -Arguments @('--build', $BuildRoot, '--target', 'dxc', 'dxcompiler', 'dxildll', '--parallel')

$BuiltDxc = Resolve-BuiltFile 'dxc.exe'
$BuiltCompiler = Resolve-BuiltFile 'dxcompiler.dll'
$BuiltDxil = Resolve-BuiltFile 'dxil.dll'
foreach ($path in @($BuiltDxc, $BuiltCompiler, $BuiltDxil)) {
    if ((Get-Item -LiteralPath $path).Length -eq 0) {
        throw "DXC build product is zero bytes: $path"
    }
}

$SmokeRoot = Join-Path ([IO.Path]::GetTempPath()) ('apusr-dxc-smoke-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $SmokeRoot | Out-Null
$SmokeHlsl = Join-Path $SmokeRoot 'smoke.hlsl'
$SmokeDxil = Join-Path $SmokeRoot 'smoke.dxil'
$SmokeSpv = Join-Path $SmokeRoot 'smoke.spv'
@'
RWByteAddressBuffer OutBuffer : register(u0);
[numthreads(1, 1, 1)]
void CS(uint3 tid : SV_DispatchThreadID) {
    OutBuffer.Store(0, tid.x);
}
'@ | Set-Content -LiteralPath $SmokeHlsl -Encoding Ascii

Write-Host '[APUSR DXC] Smoke-testing freshly built DXIL and Vulkan SPIR-V paths'
Invoke-Checked -FilePath $BuiltDxc -Arguments @('-T', 'cs_6_6', '-E', 'CS', '-Fo', $SmokeDxil, $SmokeHlsl)
Invoke-Checked -FilePath $BuiltDxc -Arguments @('-spirv', '-T', 'cs_6_6', '-E', 'CS', '-fspv-target-env=vulkan1.2', '-Fo', $SmokeSpv, $SmokeHlsl)
foreach ($path in @($SmokeDxil, $SmokeSpv)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or (Get-Item -LiteralPath $path).Length -eq 0) {
        throw "DXC smoke test did not produce valid output: $path"
    }
}

New-Item -ItemType Directory -Path $VendorBin -Force | Out-Null
$backupRoot = Join-Path ([IO.Path]::GetTempPath()) ('apusr-dxc-backup-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $backupRoot | Out-Null
foreach ($name in @('dxc.exe', 'dxcompiler.dll', 'dxil.dll')) {
    $old = Join-Path $VendorBin $name
    if (Test-Path -LiteralPath $old -PathType Leaf) {
        Copy-Item -LiteralPath $old -Destination (Join-Path $backupRoot $name) -Force
    }
}

$staged = @{
    'dxc.exe' = $BuiltDxc
    'dxcompiler.dll' = $BuiltCompiler
    'dxil.dll' = $BuiltDxil
}

try {
    foreach ($name in @('dxc.exe', 'dxcompiler.dll', 'dxil.dll')) {
        $tempPath = Join-Path $VendorBin ('.' + $name + '.apusr-new')
        Copy-Item -LiteralPath $staged[$name] -Destination $tempPath -Force
        if ((Get-FileSha256 $tempPath) -ne (Get-FileSha256 $staged[$name])) {
            throw "Staging hash mismatch for $name"
        }
    }

    foreach ($name in @('dxc.exe', 'dxcompiler.dll', 'dxil.dll')) {
        $tempPath = Join-Path $VendorBin ('.' + $name + '.apusr-new')
        Move-Item -LiteralPath $tempPath -Destination (Join-Path $VendorBin $name) -Force
    }
}
catch {
    Write-Warning 'DXC vendor replacement failed; restoring the previous matched trio.'
    foreach ($name in @('dxc.exe', 'dxcompiler.dll', 'dxil.dll')) {
        $backup = Join-Path $backupRoot $name
        if (Test-Path -LiteralPath $backup -PathType Leaf) {
            Copy-Item -LiteralPath $backup -Destination (Join-Path $VendorBin $name) -Force
        }
        $tempPath = Join-Path $VendorBin ('.' + $name + '.apusr-new')
        if (Test-Path -LiteralPath $tempPath) {
            Remove-Item -LiteralPath $tempPath -Force
        }
    }
    throw
}

$VendorDxc = Join-Path $VendorBin 'dxc.exe'
$PostDxil = Join-Path $SmokeRoot 'post-vendor.dxil'
$PostSpv = Join-Path $SmokeRoot 'post-vendor.spv'
Write-Host '[APUSR DXC] Verifying the APUSR-vendored trio in place'
try {
    Invoke-Checked -FilePath $VendorDxc -Arguments @('-T', 'cs_6_6', '-E', 'CS', '-Fo', $PostDxil, $SmokeHlsl)
    Invoke-Checked -FilePath $VendorDxc -Arguments @('-spirv', '-T', 'cs_6_6', '-E', 'CS', '-fspv-target-env=vulkan1.2', '-Fo', $PostSpv, $SmokeHlsl)
}
catch {
    Write-Warning 'Vendored DXC failed its in-place validation; restoring the previous matched trio.'
    foreach ($name in @('dxc.exe', 'dxcompiler.dll', 'dxil.dll')) {
        $backup = Join-Path $backupRoot $name
        if (Test-Path -LiteralPath $backup -PathType Leaf) {
            Copy-Item -LiteralPath $backup -Destination (Join-Path $VendorBin $name) -Force
        }
    }
    throw
}

New-Item -ItemType Directory -Path $DxcNotices -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $DxcSource 'LICENSE.TXT') -Destination (Join-Path $DxcNotices 'LICENSE.TXT') -Force
Copy-Item -LiteralPath (Join-Path $DxcSource 'ThirdPartyNotices.txt') -Destination (Join-Path $DxcNotices 'ThirdPartyNotices.txt') -Force

$versionText = ((& $VendorDxc --version 2>&1) | Out-String).Trim()
if ($LASTEXITCODE -ne 0) {
    throw 'Vendored dxc.exe failed --version after replacement.'
}

function Get-SubmoduleHead {
    param([Parameter(Mandatory = $true)][string]$RelativePath)
    $path = Join-Path $DxcSource $RelativePath
    if (-not (Test-Path -LiteralPath $path -PathType Container)) {
        return 'not-present'
    }
    return Get-GitText @('-C', $path, 'rev-parse', 'HEAD')
}

$spirvHeaders = Get-SubmoduleHead 'external\SPIRV-Headers'
$spirvTools = Get-SubmoduleHead 'external\SPIRV-Tools'
$directXHeaders = Get-SubmoduleHead 'external\DirectX-Headers'
$builtUtc = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')

$manifest = @"
APUSR vendored DXC build
=======================
Source checkout:     $DxcSource
Source commit:       $actualCommit
Source commit date:  $commitDate
SPIRV-Headers:       $spirvHeaders
SPIRV-Tools:         $spirvTools
DirectX-Headers:     $directXHeaders
Build type:          Release
Generator:           Ninja
Built UTC:           $builtUtc

Compiler version:
$versionText

Vendored SHA-256:
dxc.exe:        $(Get-FileSha256 (Join-Path $VendorBin 'dxc.exe'))
dxcompiler.dll: $(Get-FileSha256 (Join-Path $VendorBin 'dxcompiler.dll'))
dxil.dll:       $(Get-FileSha256 (Join-Path $VendorBin 'dxil.dll'))

Validation:
- Fresh-build DXIL cs_6_6 smoke compile
- Fresh-build SPIR-V cs_6_6 smoke compile targeting Vulkan 1.2
- In-place APUSR-vendored DXIL cs_6_6 smoke compile
- In-place APUSR-vendored SPIR-V cs_6_6 smoke compile targeting Vulkan 1.2
"@
Set-Content -LiteralPath $BuildInfo -Value $manifest -Encoding UTF8

Remove-Item -LiteralPath $SmokeRoot -Recurse -Force

Write-Host ''
Write-Host '[APUSR DXC] SUCCESS'
Write-Host "[APUSR DXC] Source commit: $actualCommit"
Write-Host "[APUSR DXC] Vendored trio: $VendorBin"
Write-Host "[APUSR DXC] Provenance: $BuildInfo"
Write-Host "[APUSR DXC] Previous trio backup: $backupRoot"
