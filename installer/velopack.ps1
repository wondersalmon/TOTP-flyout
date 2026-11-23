function Get-VelopackTools([string]$ToolsPath) {
    $version = '1.2.161'
    $root = Join-Path $ToolsPath "velopack-$version"
    $base = "https://github.com/velopack/velopack/releases/download/$version"
    $sdk = "velopack_libc_$version"
    $cli = "vpk.$version"
    $sdkChecksum = '31DE8F2CF05A47BD5328A9F98B0C41AEC1582156F6075F1058B26941F7F17C20'
    $cliChecksum = '2B56CE117F803FC70C103CB423BD040E395E40370F6FF6E10818E9FF9C26A323'
    $library = Join-Path $root "$sdk/lib/velopack_libc_win_x64_msvc.dll"
    $tool = Join-Path $root "$cli/tools/net9.0/any/vpk.dll"
    $include = Join-Path $root "$sdk/include"

    foreach ($dependency in @(
        @{ Asset = "$sdk.zip"; Hash = $sdkChecksum; Directory = $sdk; Entry = "$library.lib" },
        @{ Asset = "$cli.nupkg"; Hash = $cliChecksum; Directory = $cli; Entry = $tool }
    )) {
        if (Test-Path -LiteralPath $dependency.Entry -PathType Leaf) { continue }
        New-Item -ItemType Directory -Path $root -Force | Out-Null
        $archive = Join-Path $root $dependency.Asset
        Invoke-WebRequest -Uri "$base/$($dependency.Asset)" -OutFile $archive -TimeoutSec 60
        if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $dependency.Hash) {
            throw "Velopack checksum mismatch for $($dependency.Asset)."
        }
        [IO.Compression.ZipFile]::ExtractToDirectory($archive, (Join-Path $root $dependency.Directory), $true)
    }
    return @{ Include = $include; Library = $library; ImportLibrary = "$library.lib"; Tool = $tool }
}

function Build-VelopackRelease($Tools, [string]$Version, [string]$Executable, [string]$License,
                               [string]$Setup, [string]$Release) {
    $root = Join-Path (Split-Path -Parent $Release) ('velopack-' + [guid]::NewGuid().ToString('N'))
    $inputDir = Join-Path $root 'app'
    $outputDir = Join-Path $root 'output'
    New-Item -ItemType Directory -Path $inputDir, $outputDir | Out-Null
    try {
        Copy-Item -LiteralPath $Executable -Destination $inputDir
        $runtimeName = 'velopack_libc.dll'
        Copy-Item -LiteralPath (Join-Path (Split-Path -Parent $Executable) $runtimeName) -Destination $inputDir
        Copy-Item -LiteralPath $License -Destination (Join-Path $inputDir 'LICENSE.txt')
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'velopack-LICENSE.txt') -Destination $inputDir
        $appId = 'wondersalmon.TotpFlyout'
        Write-Host 'Building the Velopack per-user installer and update feed.'
        & dotnet $Tools.Tool pack --packId $appId --packTitle 'TOTP flyout' --packAuthors wondersalmon `
            --packVersion $Version --packDir $inputDir --mainExe TOTP-flyout.exe --runtime win-x64 `
            --delta None --noPortable --shortcuts StartMenuRoot --outputDir $outputDir | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "Velopack failed with exit code $LASTEXITCODE." }

        $installer = Get-ChildItem -LiteralPath $outputDir -File -Filter '*-Setup.exe'
        $feed = Join-Path $outputDir 'releases.win.json'
        $assets = (Get-Content -LiteralPath $feed -Raw | ConvertFrom-Json).Assets
        if (@($installer).Count -ne 1 -or @($assets).Count -ne 1 -or $assets[0].Type -ne 'Full' -or
            $assets[0].PackageId -ne $appId -or $assets[0].FileName -notmatch '^[A-Za-z0-9.-]+-full\.nupkg$') {
            throw 'Unexpected Velopack output.'
        }
        Copy-Item -LiteralPath $installer.FullName -Destination $Setup -Force
        foreach ($asset in 'releases.win.json', $assets[0].FileName) {
            $destination = Join-Path $Release $asset
            Copy-Item -LiteralPath (Join-Path $outputDir $asset) -Destination $destination -Force
            $destination
        }
    } finally {
        $resolved = (Resolve-Path -LiteralPath $root).Path
        if ($resolved -eq $root -and !(Get-Item -LiteralPath $root).LinkType) {
            Remove-Item -LiteralPath $root -Recurse -Force
        }
    }
}
