# KingCobra installer for Windows (PowerShell 5.1+ / 7+).
#
#   irm https://github.com/DivyanshuSinha136/kingcobra/releases/latest/download/install.ps1 | iex
#
# Downloads the Windows build, verifies its SHA-256 against the release's SHA256SUMS, unpacks it to
# %LOCALAPPDATA%\Programs\KingCobra and puts that folder on your user PATH (no administrator rights needed).
# Windows on ARM runs the x64 build through Windows' built-in emulation.
#
# Environment variables:  KC_VERSION=1.0.0   KC_REPO=owner/repo   KC_PREFIX=C:\tools\kingcobra   KC_UNINSTALL=1
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

function Install-KingCobra {
    $repo   = if ($env:KC_REPO)   { $env:KC_REPO }   else { 'DivyanshuSinha136/kingcobra' }
    $prefix = if ($env:KC_PREFIX) { $env:KC_PREFIX } else { Join-Path $env:LOCALAPPDATA 'Programs\KingCobra' }

    function Update-UserPath([string]$dir, [bool]$add) {
        $cur   = [Environment]::GetEnvironmentVariable('Path', 'User')
        $parts = @($cur -split ';' | Where-Object { $_ -and ($_.TrimEnd('\') -ne $dir.TrimEnd('\')) })
        if ($add) { $parts += $dir }
        [Environment]::SetEnvironmentVariable('Path', ($parts -join ';'), 'User')
        $env:Path = ($env:Path -split ';' | Where-Object { $_ -and ($_.TrimEnd('\') -ne $dir.TrimEnd('\')) }) -join ';'
        if ($add) { $env:Path += ";$dir" }
    }

    if ($env:KC_UNINSTALL -eq '1') {
        if (Test-Path $prefix) { Remove-Item $prefix -Recurse -Force }
        Update-UserPath $prefix $false
        Write-Host "Removed KingCobra from $prefix"
        return
    }

    $arch = $env:PROCESSOR_ARCHITECTURE
    if ($env:PROCESSOR_ARCHITEW6432) { $arch = $env:PROCESSOR_ARCHITEW6432 }
    if ($arch -eq 'ARM64') { Write-Host 'Windows on ARM64: installing the x64 build (runs through Windows emulation).' }
    elseif ($arch -ne 'AMD64') { throw "No prebuilt KingCobra for Windows/$arch. Build from source with MSYS2 (see README.md)." }

    if ($env:KC_VERSION) { $ver = $env:KC_VERSION.TrimStart('v') }
    else {
        $rel = Invoke-RestMethod "https://api.github.com/repos/$repo/releases/latest" -Headers @{ 'User-Agent' = 'kingcobra-installer' }
        $ver = $rel.tag_name.TrimStart('v')
    }
    $file = "kingcobra-$ver-windows-x64.zip"
    $base = "https://github.com/$repo/releases/download/v$ver"
    Write-Host "Installing KingCobra $ver ($file)"

    $tmp = Join-Path ([IO.Path]::GetTempPath()) ("kingcobra-" + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory $tmp | Out-Null
    try {
        Invoke-WebRequest "$base/$file" -OutFile (Join-Path $tmp $file) -UseBasicParsing
        Invoke-WebRequest "$base/SHA256SUMS" -OutFile (Join-Path $tmp 'SHA256SUMS') -UseBasicParsing
        $line = Get-Content (Join-Path $tmp 'SHA256SUMS') | Where-Object { $_ -match [regex]::Escape($file) + '$' } | Select-Object -First 1
        if (-not $line) { throw "$file is not listed in SHA256SUMS" }
        $want = ($line -split '\s+')[0].ToLower()
        $got  = (Get-FileHash (Join-Path $tmp $file) -Algorithm SHA256).Hash.ToLower()
        if ($got -ne $want) { throw "checksum mismatch for $file (expected $want, got $got) - not installing" }
        Write-Host 'Checksum OK'

        if (Test-Path $prefix) { Remove-Item $prefix -Recurse -Force }
        New-Item -ItemType Directory $prefix -Force | Out-Null
        Expand-Archive (Join-Path $tmp $file) -DestinationPath $prefix -Force
        Set-Content -Path (Join-Path $prefix 'kc.cmd') -Value "@echo off`r`n`"%~dp0kingcobra.exe`" %*" -Encoding ASCII
        Update-UserPath $prefix $true
    }
    finally { Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue }

    & (Join-Path $prefix 'kingcobra.exe') version
    Write-Host ''
    Write-Host "Installed to $prefix. Open a NEW terminal and run:  kingcobra help"
}

Install-KingCobra
