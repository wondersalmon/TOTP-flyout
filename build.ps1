[CmdletBinding()]
param(
    [string]$ToolchainRoot = (Join-Path $env:ProgramFiles 'Windhawk'),
    [string]$ReleaseTag,
    [switch]$Coverage,
    [string]$LlvmBin = (Join-Path $env:ProgramFiles 'LLVM/bin'),
    [string]$NsisCompiler,
    [switch]$Velopack = $true,
    [string]$UpdateSource = 'https://github.com/wondersalmon/TOTP-flyout/releases/latest/download'
)

$ErrorActionPreference = 'Stop'
# Preserve the literal quotes in compiler arguments such as -DWH_MOD_ID=L"totp-flyout".
$PSNativeCommandArgumentPassing = 'Standard'

$compiler = Join-Path $ToolchainRoot 'Compiler/bin/clang++.exe'
if (-not (Test-Path -LiteralPath $compiler -PathType Leaf)) {
    throw "Compiler not found at $compiler. Pass -ToolchainRoot for a portable install."
}
$engineVersion = (Get-Item -LiteralPath (Join-Path $ToolchainRoot 'windhawk.exe')).VersionInfo.ProductVersion
$engine = Join-Path $ToolchainRoot "Engine/$engineVersion"
$source = Join-Path $PSScriptRoot 'src'
$build = Join-Path $PSScriptRoot 'build'
$release = Join-Path $build 'release'
$license = Join-Path $PSScriptRoot 'LICENSE'
New-Item -ItemType Directory -Force -Path $build | Out-Null

function Invoke-Compiler {
    & $compiler @args
    if ($LASTEXITCODE -ne 0) {
        throw "Compilation failed with exit code $LASTEXITCODE."
    }
}

$mod = Get-Content -LiteralPath (Join-Path $source 'mod.cpp') -Raw
$metadata = [regex]::Match($mod, '(?m)^// @version\s+([0-9]+\.[0-9]+\.[0-9]+)\s*$')
if (-not $metadata.Success) {
    throw 'The mod metadata must supply a numeric version.'
}
$version = $metadata.Groups[1].Value
if ($ReleaseTag) {
    $tagPattern = '^v' + [regex]::Escape($version) + '(?:-[0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*)?$'
    if ($ReleaseTag -cnotmatch $tagPattern) {
        throw "Release tag '$ReleaseTag' must match mod version $version."
    }
}

if ($Velopack) {
    . (Join-Path $PSScriptRoot 'installer/velopack.ps1')
    $velo = Get-VelopackTools (Join-Path $build 'tools')
    $updateLocation = if ($UpdateSource.StartsWith('https://', [StringComparison]::OrdinalIgnoreCase)) {
        $UpdateSource
    } else {
        (Resolve-Path -LiteralPath $UpdateSource).Path.Replace('\', '/')
    }
    if ($updateLocation -match '["\x00-\x1f\\]') { throw 'Invalid update source.' }
    $standaloneFlags = @('-isystem', $velo.Include, ('-DTWOFA_UPDATE_SOURCE="{0}"' -f $updateLocation))
    $standaloneLibraries = @($velo.ImportLibrary)
} else {
    $standaloneFlags = @()
    $standaloneLibraries = @()
}

$shared = foreach ($header in 'totp', 'accounts', 'backup', 'providers', 'localization', 'appearance', 'dialogs', 'clipboard', 'menus', 'tray') {
    $content = Get-Content -LiteralPath (Join-Path $source "$header.hpp") -Raw
    ($content -replace '(?m)^#(?:pragma once|include "[^"]+")\r?\n', '').Trim()
}
$modSource = Join-Path $build 'TOTP-flyout.wh.cpp'
$licenseComment = "/*`n" + (Get-Content -LiteralPath $license -Raw).TrimEnd() + "`n*/`n"
$mod.Replace('// ==WindhawkModReadme==', "${licenseComment}`n// ==WindhawkModReadme==").Replace(
    '#include "tray.hpp"', ($shared -join "`n`n")) |
    Set-Content -LiteralPath $modSource -Encoding utf8NoBOM -NoNewline

$common = @('-std=c++23', '-O2', '-DUNICODE', '-D_UNICODE', '-DWINVER=0x0A00',
    '-D_WIN32_WINNT=0x0A00', '-D_WIN32_IE=0x0A00', '-DNTDDI_VERSION=0x0A000008',
    '-Wall', '-Wextra', '-Werror')
$systemLibraries = @('-lbcrypt', '-lshell32', '-luser32', '-lgdi32', '-ladvapi32',
    '-lcrypt32', '-lcomdlg32', '-lwtsapi32', '-ldwmapi', '-luxtheme', '-lcomctl32',
    '-lgdiplus', '-lruntimeobject')
$modFlags = @('-shared', '-DWH_MOD', '-D__USE_MINGW_ANSI_STDIO=0', '-DWH_MOD_ID=L"totp-flyout"',
    ('-DWH_MOD_VERSION=L"{0}"' -f $version), '-Wl,--export-all-symbols',
    '-Wno-unused-parameter', '-Wno-cast-function-type-mismatch')

Write-Host 'Building the 32-bit and 64-bit Windhawk mod.'
Invoke-Compiler @common @modFlags --target=i686-w64-mingw32 $modSource `
    (Join-Path $engine '32/windhawk.lib') @systemLibraries -o (Join-Path $build 'mod-32.dll')
Invoke-Compiler @common @modFlags --target=x86_64-w64-mingw32 $modSource `
    (Join-Path $engine '64/windhawk.lib') @systemLibraries -o (Join-Path $build 'mod-64.dll')

Write-Host 'Building standalone for 64-bit Windows.'
$standalone = Join-Path $build 'TOTP-flyout.exe'
Invoke-Compiler @common --target=x86_64-w64-mingw32 -static -mwindows -municode `
    @standaloneFlags (Join-Path $source 'standalone.cpp') @systemLibraries @standaloneLibraries -o $standalone
if ($Velopack) {
    # The SDK archive labels the DLL by target, while the import library uses this runtime name.
    Copy-Item -LiteralPath $velo.Library -Destination (Join-Path $build 'velopack_libc.dll') -Force
}

Write-Host 'Building and running the TOTP, account, backup, and Windows integration tests.'
$testFlags = @()
$testObjects = @()
$profiles = @()
$savedProfile = $env:LLVM_PROFILE_FILE
if ($Coverage) {
    $testFlags = @('-fprofile-instr-generate', '-fcoverage-mapping')
    $coverageDir = Join-Path $build 'coverage'
    $profdata = Join-Path $LlvmBin 'llvm-profdata.exe'
    $cov = Join-Path $LlvmBin 'llvm-cov.exe'
    if (!(Test-Path -LiteralPath $profdata) -or !(Test-Path -LiteralPath $cov)) {
        throw 'Coverage requires llvm-profdata and llvm-cov. Pass -LlvmBin with the LLVM tools directory.'
    }
    New-Item -ItemType Directory -Force -Path $coverageDir | Out-Null
}
try {
    foreach ($name in 'totp', 'account', 'windows') {
        $tests = Join-Path $build "$name-tests.exe"
        Invoke-Compiler @common @testFlags --target=x86_64-w64-mingw32 -static `
            (Join-Path $PSScriptRoot "tests/${name}_tests.cpp") @systemLibraries -o $tests
        if ($Coverage) {
            $profile = Join-Path $coverageDir "$name.profraw"
            $env:LLVM_PROFILE_FILE = $profile
            $profiles += $profile
            $testObjects += $tests
        }
        & $tests
        if ($LASTEXITCODE -ne 0) {
            throw "$name tests failed with exit code $LASTEXITCODE."
        }
    }
} finally {
    $env:LLVM_PROFILE_FILE = $savedProfile
}
if ($Coverage) {
    $indexed = Join-Path $coverageDir 'tests.profdata'
    & $profdata merge -sparse @profiles -o $indexed
    if ($LASTEXITCODE -ne 0) { throw 'Could not merge coverage profiles.' }
    $objects = @($testObjects[0]) + @($testObjects | Select-Object -Skip 1 | ForEach-Object { "-object=$_" })
    $sources = @(Get-ChildItem -LiteralPath $source -File | Select-Object -ExpandProperty FullName)
    & $cov report @objects "-instr-profile=$indexed" @sources |
        Tee-Object -FilePath (Join-Path $coverageDir 'summary.txt')
    if ($LASTEXITCODE -ne 0) { throw 'Could not generate the coverage report.' }
    & $cov export -summary-only @objects "-instr-profile=$indexed" @sources |
        Set-Content -LiteralPath (Join-Path $coverageDir 'coverage.json') -Encoding utf8NoBOM
    if ($LASTEXITCODE -ne 0) { throw 'Could not export the coverage statistics.' }
}

if (!$Velopack -and !$NsisCompiler) {
    $tools = Join-Path $build 'tools'
    $NsisCompiler = Join-Path $tools 'nsis-3.12/makensis.exe'
    if (!(Test-Path -LiteralPath $NsisCompiler -PathType Leaf)) {
        $uri = 'https://downloads.sourceforge.net/project/nsis/NSIS%203/3.12/nsis-3.12.zip'
        $mirror = 'https://distfiles.macports.org/nsis/nsis-3.12.zip'
        $expectedHash = '56581F90DB321581C5381193D796FFFCF2D24B2F8FED2160A6C6A3BAA67F2C4F'
        New-Item -ItemType Directory -Force -Path $tools | Out-Null
        $package = Join-Path $tools 'nsis-3.12.zip'
        foreach ($download in $uri, $mirror) {
            try {
                Invoke-WebRequest -Uri $download -OutFile $package -TimeoutSec 30
                if ((Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash -ne $expectedHash) {
                    throw 'NSIS download checksum mismatch.'
                }
                Expand-Archive -LiteralPath $package -DestinationPath $tools -Force
                break
            } catch {
                Write-Warning $_.Exception.Message
            }
        }
    }
}
New-Item -ItemType Directory -Force -Path $release | Out-Null
$setup = Join-Path $release 'TOTP-flyout-setup.exe'
$publishedMod = Join-Path $release (Split-Path -Leaf $modSource)
$packages = @()
if ($Velopack) {
    $packageVersion = if ($ReleaseTag) { $ReleaseTag.Substring(1) } else { $version }
    $packages = @(Build-VelopackRelease $velo $packageVersion $standalone $license $setup $release)
} else {
    if (!(Test-Path -LiteralPath $NsisCompiler -PathType Leaf)) {
        throw 'NSIS compiler not found. Pass -NsisCompiler with the path to makensis.exe.'
    }
    $NsisCompiler = (Resolve-Path -LiteralPath $NsisCompiler).Path
    Write-Host 'Building the per-user standalone installer.'
    & $NsisCompiler /WX "/DAPP_VERSION=$version" '/DAPP_PUBLISHER=wondersalmon' "/DAPP_EXE=$standalone" `
        "/DAPP_LICENSE=$license" "/DSETUP_EXE=$setup" (Join-Path $PSScriptRoot 'installer/setup.nsi')
    if ($LASTEXITCODE -ne 0) { throw "NSIS failed with exit code $LASTEXITCODE." }
}
Copy-Item -LiteralPath $modSource -Destination $publishedMod -Force
Get-ChildItem -LiteralPath $release -File | Where-Object {
    ($_.Name -eq 'releases.win.json' -or $_.Name -match '^wondersalmon\.TotpFlyout-[0-9][0-9A-Za-z.-]*-full\.nupkg$') -and
        $_.FullName -notin $packages
} | Remove-Item
$publishedFiles = @($publishedMod, $setup) + $packages
$checksums = Get-FileHash -LiteralPath $publishedFiles -Algorithm SHA256 | ForEach-Object {
    '{0}  {1}' -f $_.Hash.ToLowerInvariant(), (Split-Path -Leaf $_.Path)
}
[IO.File]::WriteAllText((Join-Path $release 'SHA256SUMS.txt'), ($checksums -join "`n") + "`n")
Write-Host "Release files are in $release."
