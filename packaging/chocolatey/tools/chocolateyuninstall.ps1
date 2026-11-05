$ErrorActionPreference = 'Stop'
$toolsDir = Split-Path -Parent $MyInvocation.MyCommand.Definition

# Chocolatey removes the unzipped files and the `kingcobra` shim itself; the extra `kc` shim is ours to remove.
Uninstall-BinFile -Name 'kc' -Path (Join-Path $toolsDir 'kingcobra.exe')
