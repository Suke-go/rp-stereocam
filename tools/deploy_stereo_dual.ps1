<#
.SYNOPSIS
    Deploy and build the isolated Dual stereo sender tree on Raspberry Pi.

.DESCRIPTION
    Copies only native/imt, native/stream_sender, and the Dual launch script.
    The default destination is separate from ~/MetaPuppet so an existing dirty
    Pi checkout is not overwritten. This script builds but does not start the
    cameras or sender.

.EXAMPLE
    pwsh tools/deploy_stereo_dual.ps1
#>

param(
    [string]$PiHost = "100.96.88.119",
    [string]$PiUser = "admin",
    [string]$RemotePath = "/home/admin/MetaPuppet-dual",
    [int]$BuildJobs = 4
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$target = "$PiUser@$PiHost"
$sshOptions = @(
    "-o", "BatchMode=yes",
    "-o", "StrictHostKeyChecking=accept-new",
    "-o", "ConnectTimeout=10"
)

Write-Host "Deploying Dual sender sources to $($target):$RemotePath"
& ssh @sshOptions $target "mkdir -p '$RemotePath/native' '$RemotePath/tools'"
if ($LASTEXITCODE -ne 0) { throw "Failed to create the remote deployment directory." }

$nativeSources = @(
    (Join-Path $repoRoot "native/imt"),
    (Join-Path $repoRoot "native/stream_sender")
)
& scp @sshOptions -r @nativeSources "$($target):$RemotePath/native/"
if ($LASTEXITCODE -ne 0) { throw "Failed to copy native sender sources." }

& scp @sshOptions (Join-Path $repoRoot "tools/run_stereo_dual.sh") "$($target):$RemotePath/tools/"
if ($LASTEXITCODE -ne 0) { throw "Failed to copy the Dual launch script." }

# Never overwrite device-local capture tuning during a source deployment.
# Stage only a reference file when the operator needs to create/update it.
& scp @sshOptions (Join-Path $repoRoot "tools/metapuppet_pi.env") `
    "$($target):$RemotePath/tools/metapuppet_pi.env.example"
if ($LASTEXITCODE -ne 0) { throw "Failed to copy the capture config example." }

$remoteBuild = "set -eu; " +
    "sed -i 's/\r$//' '$RemotePath/tools/run_stereo_dual.sh'; " +
    "chmod +x '$RemotePath/tools/run_stereo_dual.sh'; " +
    "cmake -S '$RemotePath/native/stream_sender' -B '$RemotePath/build/pi_stream_sender' " +
    "-G Ninja -DCMAKE_BUILD_TYPE=Release -DMPS_ENABLE_X264=ON; " +
    "cmake --build '$RemotePath/build/pi_stream_sender' " +
    "--target mps_stereo_x264_sender -j$BuildJobs"
& ssh @sshOptions $target $remoteBuild
if ($LASTEXITCODE -ne 0) { throw "Remote Dual sender build failed." }

Write-Host "Deployment complete. The sender was not started."
Write-Host "Run it with:"
Write-Host "  ssh $target"
Write-Host "  cd $RemotePath"
Write-Host "  ./tools/run_stereo_dual.sh 192.168.50.1 5004 1280 720 60 0 1"
