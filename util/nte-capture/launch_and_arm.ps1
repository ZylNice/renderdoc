# One-click "arm a running NTE launcher" flow (alternative to the pure-UI flow).
#
# What it does:
#   1. Stops all NTE launcher-related processes (incl. the updater that self-heals CRT files).
#   2. Replaces the launcher's bundled old CRT with the system CRT (backup kept in crt_backup\).
#      NOTE: since the injected rendertest.dll now does this replacement automatically at every
#      launch / child-process spawn (see Process::FixBundledCRTForTarget in win32_process.cpp),
#      this step is belt-and-suspenders - it guarantees a good CRT even for the very first process.
#   3. Starts the launcher and injects rendertest.dll into it with --opt-hook-children, so the
#      whole chain (stub -> NTELauncher -> NTEUpdate -> real UI -> game) gets injected and you can
#      click "start game" to capture.
#
# Adjust the two paths below for your machine before running.
$launcher = "C:\GAME\Neverness To Everness\NTELauncher\NTEGame.exe"
$cmd      = "C:\Project\Renderdoc\renderdoc\x64\Development\rendertestcmd.exe"
$rt       = "C:\GAME\Neverness To Everness\NTELauncher\runtime"
$bk       = Join-Path $rt "crt_backup"
$dlls     = @("msvcp140.dll","msvcp140_1.dll","msvcp140_2.dll","vcruntime140.dll","vcruntime140_1.dll","ucrtbase.dll")

# 0. stop ALL launcher-related processes (incl. updater which self-heals the CRT files)
Get-Process -Name "NTEGame","NTEBrowser","NTEWebBooster","NTELauncher","NTEUpdate","NTEErrRep" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2

# 1. replace bundled CRT with system CRT (backup originals once)
if(-not (Test-Path $bk)) { New-Item -ItemType Directory -Path $bk -Force | Out-Null }
foreach($d in $dlls) {
    $src = Join-Path $rt $d
    $bakFile = Join-Path $bk $d
    if((Test-Path $src) -and -not (Test-Path $bakFile)) { Copy-Item $src $bakFile -Force }
    Copy-Item (Join-Path "C:\windows\System32" $d) $src -Force
}

# verify replacement really took effect
$v = (Get-Item (Join-Path $rt "msvcp140.dll")).VersionInfo.FileVersion
if($v -notlike "14.5*") { Write-Output "CRT_REPLACE_FAILED version=$v"; exit 1 }
Write-Output "CRT replaced OK (msvcp140 = $v)"

# 2. start launcher
Write-Output "Starting launcher..."
Start-Process $launcher
$proc = $null
for($i = 0; $i -lt 40; $i++) {
    Start-Sleep -Seconds 1
    $proc = Get-Process -Name "NTEGame" -ErrorAction SilentlyContinue | Sort-Object StartTime -Descending | Select-Object -First 1
    if($proc) { break }
}
if(-not $proc) { Write-Output "LAUNCHER_NOT_FOUND"; exit 1 }
Write-Output "Launcher PID $($proc.Id), waiting briefly for stable state..."
Start-Sleep -Seconds 4
$proc = Get-Process -Id $proc.Id -ErrorAction SilentlyContinue
if(-not $proc) { Write-Output "LAUNCHER_EXITED"; exit 1 }

# 3. inject with child-hooking
Write-Output "Injecting rendertest.dll into launcher PID $($proc.Id)..."
& $cmd inject --PID=$($proc.Id) --opt-hook-children 2>&1
