# Transport harness (Windows) -- boots the real SuperSonic binary once per
# command transport (TCP, named pipe, SHM), sends /status over that transport
# with the transport_probe client, and requires a reply. Headless throughout.
# The secret goes to the server's stdin at boot and to the probe's, as a
# launcher would give it to both.
#
#   powershell -File test\transport-harness\run.ps1 [path-to-SuperSonic.exe]
#
# Exit 0 = every transport answered. macOS/Linux (TCP/UDS/SHM) is run.sh.
#
# CI runs it on the windows-x64 runner; it has also run on Windows on ARM.
param([string]$Bin = "")

$ErrorActionPreference = "Stop"
$Repo = Resolve-Path (Join-Path $PSScriptRoot "..\..")
if ($Bin -eq "") { $Bin = Join-Path $Repo "build\native\Release\SuperSonic.exe" }
if (-not (Test-Path $Bin)) { Write-Error "harness: binary not found: $Bin"; exit 2 }

Write-Host "harness: building transport_probe..."
Push-Location (Join-Path $Repo "clockwork\rust")
cargo build -q --example transport_probe -p clockwork-comms
if ($LASTEXITCODE -ne 0) { Pop-Location; exit 2 }
Pop-Location
$Probe = Join-Path $Repo "clockwork\rust\target\debug\examples\transport_probe.exe"

$Failures = 0
# The secret, as a file both processes read as their stdin.
$SecretFile = Join-Path $env:TEMP "ss-harness-secret-$PID.txt"
$Secret = -join ((1..32) | ForEach-Object { '{0:x2}' -f (Get-Random -Minimum 0 -Maximum 256) })
Set-Content -Path $SecretFile -Value $Secret -NoNewline
Add-Content -Path $SecretFile -Value "`n" -NoNewline

function Check($Name, $Proto, $Target, $ServerArgs) {
    $log = Join-Path $env:TEMP "ss-harness-$Name.log"
    $proc = Start-Process -FilePath $Bin -ArgumentList (@("--headless", "-u", "0") + $ServerArgs) `
        -RedirectStandardInput $SecretFile `
        -RedirectStandardError $log -RedirectStandardOutput "$log.out" -PassThru -NoNewWindow
    $ok = $false
    # The server binds its transport shortly after boot, so the first probes
    # can find nothing there: their complaints are kept, and shown only if the
    # server never answers (as run.sh does), rather than read as failures.
    $smokeErr = "$log.probe"
    foreach ($i in 1..20) {
        # The probe blocks <=3s itself; treat a hung pipe read as failure too.
        $p = Start-Process -FilePath $Probe -ArgumentList @($Proto, $Target) `
            -RedirectStandardInput $SecretFile `
            -RedirectStandardError $smokeErr -PassThru -NoNewWindow -Wait
        if ($p.ExitCode -eq 0) { $ok = $true; break }
        if ($proc.HasExited) { break }
        Start-Sleep -Milliseconds 500
    }
    if (-not $ok -and (Test-Path $smokeErr)) { Get-Content $smokeErr | ForEach-Object { Write-Host $_ } }
    # Server is up (smoke answered) -> medium end-to-end load blast: many /sync
    # ids through the real engine, verify the /synced replies.
    if ($ok) {
        $lp = Start-Process -FilePath $Probe -ArgumentList @("load", $Proto, $Target, "$LoadCount") `
            -RedirectStandardInput $SecretFile -PassThru -NoNewWindow -Wait
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

$TcpPort = Get-Random -Minimum 20000 -Maximum 40000
$ShmPort = Get-Random -Minimum 20000 -Maximum 40000
$Pipe    = "ss-harness-$PID"

Check "tcp"  "tcp"  "127.0.0.1:$TcpPort" @("--tcp", "$TcpPort", "-B", "127.0.0.1")
Check "pipe" "pipe" $Pipe                @("--pipe", $Pipe)
# SHM command plane: --shm-commands needs -u > 0 (the port names the segment);
# the trailing -u overrides the -u 0 in Check (last flag wins).
Check "shm"  "shm"  "$ShmPort"           @("-u", "$ShmPort", "--shm-commands")

Remove-Item -Path $SecretFile -ErrorAction SilentlyContinue
if ($Failures -gt 0) { Write-Host "harness: $Failures transport(s) failed"; exit 1 }
Write-Host "harness: all transports answered"
