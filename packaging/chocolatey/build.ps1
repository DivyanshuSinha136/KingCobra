<#
 Builds kingcobra.<version>.nupkg from the templates in this directory.

   ./packaging/chocolatey/build.ps1 -Version 1.0.0 -Repo owner/repo -ZipPath .\kingcobra-1.0.0-windows-x64.zip
   ./packaging/chocolatey/build.ps1 -Version 1.0.0 -Repo owner/repo -Url <zip url> -Sha256 <hash>

 -ZipPath is used to compute the SHA-256 (and the download URL is derived from -Repo / -Version unless -Url is
 given). Output goes to -OutDir (default: choco-out). Needs `choco` on PATH.
#>
param(
  [Parameter(Mandatory)][string]$Version,
  [Parameter(Mandatory)][string]$Repo,
  [string]$ZipPath,
  [string]$Url,
  [string]$Sha256,
  [string]$OutDir = 'choco-out'
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Definition

if (-not $Url) { $Url = "https://github.com/$Repo/releases/download/v$Version/kingcobra-$Version-windows-x64.zip" }
if (-not $Sha256) {
  if (-not $ZipPath) { throw 'give -Sha256, or -ZipPath so it can be computed' }
  $Sha256 = (Get-FileHash -Algorithm SHA256 -Path $ZipPath).Hash.ToLower()
}

$stage = Join-Path ([IO.Path]::GetTempPath()) ("kc-choco-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force (Join-Path $stage 'tools') | Out-Null

function Fill([string]$text) {
  $text.Replace('@VERSION@', $Version).Replace('@REPO@', $Repo).Replace('@URL@', $Url).Replace('@SHA256@', $Sha256)
}
$utf8 = New-Object System.Text.UTF8Encoding($false)
[IO.File]::WriteAllText((Join-Path $stage 'kingcobra.nuspec'),
  (Fill ([IO.File]::ReadAllText((Join-Path $here 'kingcobra.nuspec.in')))), $utf8)
[IO.File]::WriteAllText((Join-Path $stage 'tools\chocolateyinstall.ps1'),
  (Fill ([IO.File]::ReadAllText((Join-Path $here 'tools\chocolateyinstall.ps1.in')))), $utf8)
Copy-Item (Join-Path $here 'tools\chocolateyuninstall.ps1') (Join-Path $stage 'tools\chocolateyuninstall.ps1')

New-Item -ItemType Directory -Force $OutDir | Out-Null
$out = (Resolve-Path $OutDir).Path
Push-Location $stage
try { choco pack --out $out; if ($LASTEXITCODE -ne 0) { throw "choco pack failed ($LASTEXITCODE)" } }
finally { Pop-Location }
Remove-Item -Recurse -Force $stage
$nupkg = Join-Path $out "kingcobra.$Version.nupkg"
Write-Host "Built: $nupkg  (zip sha256 $Sha256)"
