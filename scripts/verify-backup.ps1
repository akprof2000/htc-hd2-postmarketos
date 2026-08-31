# Сверка бэкапа SD-карты с оригиналом. Запускать при вставленной карте.
#   powershell -File scripts\verify-backup.ps1 -Card D:\ -Backup C:\Projects\HTC-HD2-T8585\backup\sdcard
param(
  [string]$Card   = 'D:\',
  [string]$Backup = 'C:\Projects\HTC-HD2-T8585\backup\sdcard'
)
$ErrorActionPreference = 'Stop'
$skip = @('System Volume Information','LOST.DIR')

function Get-Tree($root) {
  Get-ChildItem -LiteralPath $root -Recurse -File -Force -ErrorAction SilentlyContinue |
    Where-Object { $p = $_.FullName.Substring($root.TrimEnd('\').Length).TrimStart('\')
                   -not ($skip | Where-Object { $p -like "$_\*" -or $p -eq $_ }) } |
    ForEach-Object { [pscustomobject]@{
        Rel  = $_.FullName.Substring($root.TrimEnd('\').Length).TrimStart('\')
        Size = $_.Length } }
}

$src = Get-Tree $Card
$dst = Get-Tree $Backup
Write-Output "Карта:  $($src.Count) файлов, $([math]::Round(($src|Measure-Object Size -Sum).Sum/1GB,3)) ГБ"
Write-Output "Бэкап:  $($dst.Count) файлов, $([math]::Round(($dst|Measure-Object Size -Sum).Sum/1GB,3)) ГБ"

$map = @{}; $dst | ForEach-Object { $map[$_.Rel] = $_.Size }
$missing = $src | Where-Object { -not $map.ContainsKey($_.Rel) }
$badsize = $src | Where-Object { $map.ContainsKey($_.Rel) -and $map[$_.Rel] -ne $_.Size }

if ($missing) { Write-Output "`nНЕ СКОПИРОВАНО ($($missing.Count)):"; $missing | Select-Object -First 20 Rel,Size | Format-Table -AutoSize }
if ($badsize) { Write-Output "`nРАЗМЕР НЕ СОВПАЛ ($($badsize.Count)):"; $badsize | Select-Object -First 20 Rel,Size | Format-Table -AutoSize }

Write-Output "`nКонтрольные суммы ICS (то, без чего HaRET не запустится):"
$ok = $true
foreach ($f in 'ICS\STARTUP.TXT','ICS\haret.exe','ICS\clrcad.exe','ICS\zImage','ICS\initrd.gz','ICS\system.ext4','ICS\data.ext4') {
  $a = Join-Path $Card $f; $b = Join-Path $Backup $f
  if (-not (Test-Path -LiteralPath $b)) { Write-Output ("  {0,-20} ОТСУТСТВУЕТ В БЭКАПЕ" -f $f); $ok = $false; continue }
  $ha = (Get-FileHash -LiteralPath $a -Algorithm SHA256).Hash
  $hb = (Get-FileHash -LiteralPath $b -Algorithm SHA256).Hash
  $verdict = if ($ha -eq $hb) { 'OK' } else { $ok = $false; 'РАСХОЖДЕНИЕ' }
  Write-Output ("  {0,-20} {1}  {2}" -f $f, $verdict, $ha.Substring(0,16))
}

if (-not $missing -and -not $badsize -and $ok) {
  Write-Output "`nБЭКАП ПОЛНЫЙ. Переразмечать карту можно."
} else {
  Write-Output "`nБЭКАП НЕПОЛНЫЙ. Карту не трогать, повторить копирование."
  exit 1
}
