# Transport harness (Windows) -- boots the real SuperSonic binary once per
# command transport (UDP, TCP, named pipe), sends /status over that transport
# with the transport_probe client, and requires a reply. Headless throughout.
#
#   powershell -File test\transport-harness\run.ps1 [path-to-SuperSonic.exe]
#
# Exit 0 = every transport answered. macOS/Linux (UDP/TCP/UDS) is run.sh.
#
# WARNING: not yet exercised on a real Windows machine (CI runs it on the
# windows-x64 runner).
param([string]$Bin = "")

$ErrorActionPreference = "Stop"
$Repo = Resolve-Path (Join-Path $PSScriptRoot "..\..")
if ($Bin -eq "") { $Bin = Join-Path $Repo "build\native\SuperSonic_artefacts\Release\SuperSonic.exe" }
if (-not (Test-Path $Bin)) { Write-Error "harness: binary not found: $Bin"; exit 2 }

Write-Host "harness: building transport_probe..."
Push-Location (Join-Path $Repo "rust")
cargo build -q --example transport_probe -p supersonic-osc-net
if ($LASTEXITCODE -ne 0) { Pop-Location; exit 2 }
Pop-Location
$Probe = Join-Path $Repo "rust\target\debug\examples\transport_probe.exe"

$Failures = 0

function Check($Name, $Proto, $Target, $ServerArgs) {
    $log = Join-Path $env:TEMP "ss-harness-$Name.log"
    $proc = Start-Process -FilePath $Bin -ArgumentList (@("--headless", "-u", "0") + $ServerArgs) `
        -RedirectStandardError $log -RedirectStandardOutput "$log.out" -PassThru -NoNewWindow
    $ok = $false
    foreach ($i in 1..20) {
        # The probe blocks <=3s itself; treat a hung pipe read as failure too.
        $p = Start-Process -FilePath $Probe -ArgumentList @($Proto, $Target) `
            -PassThru -NoNewWindow -Wait
        if ($p.ExitCode -eq 0) { $ok = $true; break }
        if ($proc.HasExited) { break }
        Start-Sleep -Milliseconds 500
    }
    # Server is up (smoke answered) -> medium end-to-end load blast: many /sync
    # ids through the real engine, verify the /synced replies.
    if ($ok) {
        $lp = Start-Process -FilePath $Probe -ArgumentList @("load", $Proto, $Target, "$LoadCount") `
            -PassThru -NoNewWindow -Wait
        if ($lp.ExitCode -ne 0) { $ok = $false }
    }
    if ($ok) {
        Write-Host "PASS $Name"
    } else {
        Write-Host "FAIL $Name -- server log:"
        if (Test-Path $log) { Get-Content $log | ForEach-Object { Write-Host "  | $_" } }
        $script:Failures++
    }
    if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
}

# Medium end-to-end load per transport (see run.sh for rationale).
$LoadCount = 5000

$UdpPort = Get-Random -Minimum 20000 -Maximum 40000
$TcpPort = Get-Random -Minimum 20000 -Maximum 40000
$ShmPort = Get-Random -Minimum 20000 -Maximum 40000
$Pipe    = "ss-harness-$PID"

Check "udp"  "udp"  "127.0.0.1:$UdpPort" @("-u", "$UdpPort")
Check "tcp"  "tcp"  "127.0.0.1:$TcpPort" @("--tcp", "$TcpPort", "-B", "127.0.0.1")
Check "pipe" "pipe" $Pipe                @("--pipe", $Pipe)
# SHM command plane: --shm-commands needs -u > 0 (the port names the segment);
# the trailing -u overrides the -u 0 in Check (last flag wins, as for udp).
Check "shm"  "shm"  "$ShmPort"           @("-u", "$ShmPort", "--shm-commands")

if ($Failures -gt 0) { Write-Host "harness: $Failures transport(s) failed"; exit 1 }
Write-Host "harness: all transports answered"
