param([string]$Dll = "KCtest10.dll")

$dir = "C:\Users\Meow\tools\KCtest\KCtestMini"

$p = Get-Process Metin2_windows-x64 -ErrorAction Stop
$clid = $p.Id
Write-Host "client pid=$clid"

function Test-LiveFile([string]$name) {
    $it = Get-Item (Join-Path $dir $name) -ErrorAction SilentlyContinue
    if (-not $it) { return $false }
    $t = [string](Get-Content $it.FullName -Raw -ErrorAction SilentlyContinue)
    if ($t) {
        $parts = $t.Trim() -split '\s+'
        if ($parts.Count -ge 1 -and $parts[0] -ne "$clid") { return $false }
    }
    return (((Get-Date) - $it.LastWriteTime).TotalSeconds -lt 4)
}

# 1. every bot copy must be stopped: two copies sending attack packets
#    into the same client fight each other.
New-Item -Force (Join-Path $dir "quit.txt") | Out-Null
New-Item -Force (Join-Path $dir "walkatk_quit.txt") | Out-Null
New-Item -Force (Join-Path $dir "kcquit.txt") | Out-Null
$deadline = (Get-Date).AddSeconds(30)
while ((Test-LiveFile "aoe_alive.txt") -or (Test-LiveFile "kc_alive.txt")) {
    if ((Get-Date) -ge $deadline) { break }
    Start-Sleep -Milliseconds 400
}
Remove-Item (Join-Path $dir "quit.txt") -ErrorAction SilentlyContinue
Remove-Item (Join-Path $dir "walkatk_quit.txt") -ErrorAction SilentlyContinue
Remove-Item (Join-Path $dir "kcquit.txt") -ErrorAction SilentlyContinue
if ((Test-LiveFile "aoe_alive.txt") -or (Test-LiveFile "kc_alive.txt")) {
    Write-Host "ECHEC: une copie est encore en vie dans $clid. Ne pas injecter dessus." -ForegroundColor Red
    exit 1
}

# 2. unload modules still mapped (CreateRemoteThread ref-count).
$p.Refresh()
$mods = @($p.Modules | Where-Object {
    $_.ModuleName -like "aoe*.dll" -or
    $_.ModuleName -like "walkatk*.dll" -or
    $_.ModuleName -like "KCtest*.dll"
})
foreach ($m in $mods) {
    $b = '{0:X}' -f $m.BaseAddress.ToInt64()
    Write-Host "unload $($m.ModuleName) base=$b"
    & (Join-Path $dir "unload.exe") $clid $b
}
if ($mods.Count -gt 0) { Start-Sleep -Milliseconds 800 }
$p.Refresh()
$still = @($p.Modules | Where-Object { $_.ModuleName -like "KCtest*.dll" })
if ($still.Count -gt 0 -and $still[0].ModuleName -eq $Dll) {
    Write-Host "ECHEC: $Dll est toujours mappee (base $($still[0].BaseAddress)); changer de nom." -ForegroundColor Red
    exit 1
}

# 3. inject
& (Join-Path $dir "inject.exe") (Join-Path $dir $Dll) $clid

# 4. the new copy announces itself
$deadline = (Get-Date).AddSeconds(20)
while (-not (Test-LiveFile "kc_alive.txt")) {
    if ((Get-Date) -ge $deadline) { break }
    Start-Sleep -Milliseconds 300
}
if (Test-LiveFile "kc_alive.txt") {
    Write-Host "vivant: $(Get-Content (Join-Path $dir 'kc_alive.txt') -Raw)"
} else {
    Write-Host "ATTENTION: aucun fichier de vie 20 s apres l'injection" -ForegroundColor Yellow
}

Start-Sleep -Seconds 1
$p.Refresh()
$p.Modules | Where-Object {
    $_.ModuleName -like "aoe*.dll" -or
    $_.ModuleName -like "walkatk*.dll" -or
    $_.ModuleName -like "KCtest*.dll"
} | Select-Object ModuleName, BaseAddress | Format-Table -AutoSize
Write-Host "--- kctest.log ---"
Get-Content (Join-Path $dir "kctest.log") -Tail 15 -ErrorAction SilentlyContinue
Write-Host "--- kctest.txt (on=0 -> passe a on=1 pour tester) ---"
Get-Content (Join-Path $dir "kctest.txt") -ErrorAction SilentlyContinue