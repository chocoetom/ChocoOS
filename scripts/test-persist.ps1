# Two-boot persistence test for the writable volume.
# Boot 1 creates build/chocovol.img and writes keep.txt; boot 2 reopens it and
# must read the same bytes back. Requires build/ChocoOS.iso (build-local.ps1).
$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $root

$qemu = "C:\msys64\ucrt64\bin\qemu-system-x86_64.exe"
$python = "C:\Python314\python.exe"
$iso = "build/ChocoOS.iso"
$vol = "build/chocovol.img"
$log = "build/serial.log"

if (-not (Test-Path $iso)) { throw "no ISO: run scripts/build-local.ps1 first" }
if (-not (Test-Path $qemu)) { throw "qemu not found at $qemu" }

# Fresh volume so the test is deterministic.
Remove-Item $vol -Force -ErrorAction SilentlyContinue
# A missing volume image makes QEMU abort before the guest starts.
if (-not (Test-Path $vol)) {
    $fs = [System.IO.File]::Create($vol)
    $fs.SetLength(512 * 1024)
    $fs.Close()
}

function Invoke-Boot([string]$Command) {
    Remove-Item $log -Force -ErrorAction SilentlyContinue
    $args = @(
        "-drive", "format=raw,file=$iso,if=ide,index=0,media=cdrom",
        "-drive", "format=raw,file=$vol,if=ide,index=2,media=disk",
        "-m", "256M",
        "-accel", "tcg,tb-size=64",
        "-serial", "file:$log",
        "-display", "none",
        "-no-reboot",
        "-qmp", "tcp:127.0.0.1:4444,server=on,wait=off"
    )
    $proc = Start-Process -FilePath $qemu -ArgumentList $args -WorkingDirectory $root -PassThru
    # QMP can take a while to accept connections on a cold boot; retry.
    $ready = $false
    for ($i = 0; $i -lt 30; $i++) {
        Start-Sleep -Seconds 1
        try {
            $probe = New-Object System.Net.Sockets.TcpClient
            $probe.Connect("127.0.0.1", 4444)
            $probe.Close()
            $ready = $true
            break
        } catch { }
    }
    if (-not $ready) {
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        throw "QEMU QMP did not become ready"
    }
    Start-Sleep -Seconds 8
    $script = @'
import json, socket, time, sys
cmd = sys.argv[1]
s = socket.create_connection(("127.0.0.1", 4444), 15)
f = s.makefile("r"); f.readline()
def call(x):
    s.sendall((json.dumps(x) + "\n").encode()); return f.readline()
call({"execute": "qmp_capabilities"})
table = {"\n": "ret", " ": "spc", "/": "slash"}
for ch in cmd:
    call({"execute": "send-key", "arguments": {
        "keys": [{"type": "qcode", "data": table.get(ch, ch)}]}})
    time.sleep(0.05)
time.sleep(5)
'@
    Set-Content -Path build\_persist_key.py -Value $script
    & $python build\_persist_key.py $Command | Out-Null
    Remove-Item build\_persist_key.py -Force -ErrorAction SilentlyContinue
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1
    if (-not (Test-Path $log)) { return "" }
    return (Get-Content $log -Raw)
}

Write-Host "[persist] boot 1: creating keep.txt ..."
$out1 = Invoke-Boot "run bin/lpersist`n"
if ($out1 -notmatch "first boot") {
    Write-Host $out1
    throw "boot 1 did not create the file"
}
if ($out1 -notmatch "\[vol\] formatted persistent volume") {
    Write-Host "[persist] note: volume was not formatted (no disk?)"
}

Write-Host "[persist] boot 2: reading keep.txt back ..."
$out2 = Invoke-Boot "run bin/lpersist`n"
Write-Host $out2
if ($out2 -match "persist: PASS") {
    Write-Host "[persist] PASS - file survived the reboot"
    exit 0
}
exit 1
