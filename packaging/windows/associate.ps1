<#
 Associates *.kc files with KingCobra and gives every .kc file the KingCobra icon.
 Per-user (HKCU): no administrator rights needed.   Usage:
     powershell -ExecutionPolicy Bypass -File packaging\windows\associate.ps1 -Exe C:\path\to\kingcobra.exe -Icon C:\path\to\kingcobra.ico
     powershell -ExecutionPolicy Bypass -File packaging\windows\associate.ps1 -Remove
 (`make install-windows` runs this for you.)
#>
param(
  [string]$Exe  = (Join-Path (Get-Location) "kingcobra.exe"),
  [string]$Icon = (Join-Path (Get-Location) "assets\icons\kingcobra.ico"),
  [switch]$Remove
)
$classes = "HKCU:\Software\Classes"
if ($Remove) {
  Remove-Item -Recurse -Force "$classes\.kc", "$classes\KingCobra.Source" -ErrorAction SilentlyContinue
} else {
  $Exe  = (Resolve-Path $Exe).Path
  $Icon = (Resolve-Path $Icon).Path
  New-Item -Force "$classes\.kc" | Out-Null
  Set-ItemProperty "$classes\.kc" "(default)" "KingCobra.Source"
  New-Item -Force "$classes\KingCobra.Source" | Out-Null
  Set-ItemProperty "$classes\KingCobra.Source" "(default)" "KingCobra source file"
  New-Item -Force "$classes\KingCobra.Source\DefaultIcon" | Out-Null
  Set-ItemProperty "$classes\KingCobra.Source\DefaultIcon" "(default)" $Icon
  New-Item -Force "$classes\KingCobra.Source\shell\open\command" | Out-Null
  Set-ItemProperty "$classes\KingCobra.Source\shell\open\command" "(default)" "`"$Exe`" run `"%1`""
}
# tell Explorer to refresh its icon cache
Add-Type -Namespace KC -Name Shell -MemberDefinition '[DllImport("shell32.dll")] public static extern void SHChangeNotify(int e, int f, IntPtr a, IntPtr b);'
[KC.Shell]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)
Write-Host ("KingCobra file association " + $(if ($Remove) { "removed" } else { "installed" }))
