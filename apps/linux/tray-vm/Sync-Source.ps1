# Pushes the working tree into the guest at ~/src.
#
#   .\Sync-Source.ps1
#
# What git would track, plus whatever is uncommitted: `git ls-files -co
# --exclude-standard` is the file list, so a half-finished fix on the host is
# what the guest builds, and build directories and other ignored output are
# not carried across. Fifty megabytes over a NAT link, a few seconds.
[CmdletBinding()]
param(
    [string]$VmDir = 'D:\Virtual Machines\Crucible Tray Test',
    [string]$Name = 'Crucible Tray Test',
    [string]$Repo = (Join-Path $PSScriptRoot '..\..\..'),
    [string]$Address
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\TrayVmCommon.ps1"
$vm = Get-TrayVm -VmDir $VmDir -Name $Name
if (-not $Address) { $Address = Get-TrayVmAddress $vm }
$Repo = (Resolve-Path $Repo).Path

$tar = Join-Path $env:TEMP 'crucible-src.tar'
$list = Join-Path $env:TEMP 'crucible-src.files'
Push-Location $Repo
try {
    & git ls-files -co --exclude-standard | Set-Content $list -Encoding ASCII
    if ($LASTEXITCODE -ne 0) { throw 'git ls-files failed' }
    & tar -cf $tar -T $list
    if ($LASTEXITCODE -ne 0) { throw 'tar failed' }
} finally { Pop-Location }
Write-Host ("staged {0:N1} MB from {1}" -f ((Get-Item $tar).Length / 1MB), $Repo)

& scp @(Get-TraySshArgs $vm) $tar "$($vm.User)@${Address}:/tmp/crucible-src.tar"
if ($LASTEXITCODE -ne 0) { throw 'scp failed' }
# Extracted over the top rather than into a fresh directory, so an
# incremental build keeps its object files; the pristine Main.qml
# build-crucible.sh keeps is removed with it so the next build re-takes one
# from what was just pushed.
# CRLF off the build inputs. This repository is developed on Windows with
# core.autocrlf on, so its working tree is CRLF and a tar of it carries that
# into the guest. The compiler and CMake do not mind, but a Linux checkout
# would not look like this, and an anchored sed - which is what the
# reproducer edit is - matches nothing against a line ending in \r. Only the
# files this build reads, by extension: test fixtures are checksummed text in
# places and none of them is built here.
$extract = @'
mkdir -p ~/src
rm -f ~/src/apps/crucible/ui/qml/Main.qml.pristine
tar -xf /tmp/crucible-src.tar -C ~/src
rm /tmp/crucible-src.tar
find ~/src \( -name '*.qml' -o -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \
           -o -name '*.cmake' -o -name 'CMakeLists.txt' -o -name '*.sh' \) \
     -print0 | xargs -0 sed -i 's/\r$//'
echo "~/src: $(find ~/src -type f | wc -l) files"
'@
Invoke-TrayGuest $vm $extract -Address $Address
Remove-Item $tar, $list -Force
