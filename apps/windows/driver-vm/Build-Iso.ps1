# Writes a small ISO 9660 + Joliet image from a directory, using Windows'
# own IMAPI2 (no oscdimg or mkisofs needed). Used to carry autounattend.xml
# and the driver package into the test guest as virtual CDs.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Source,
    [Parameter(Mandatory)][string]$Out,
    [string]$Label = 'DATA'
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Source)) { throw "no such directory: $Source" }

# IMAPI2 hands back a COM IStream; copying it to a file is done in a few
# lines of C# because PowerShell cannot call IStream::Read with the byref
# count the interface wants.
if (-not ('Ac3Forge.IsoWriter' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
namespace Ac3Forge {
    public static class IsoWriter {
        public static void Write(string path, object imageStream, int blockSize, int totalBlocks) {
            IStream stream = (IStream)imageStream;
            byte[] buffer = new byte[blockSize];
            IntPtr read = Marshal.AllocHGlobal(4);
            try {
                using (FileStream file = new FileStream(path, FileMode.Create, FileAccess.Write)) {
                    for (int block = 0; block < totalBlocks; ++block) {
                        stream.Read(buffer, blockSize, read);
                        int got = Marshal.ReadInt32(read);
                        if (got <= 0) { break; }
                        file.Write(buffer, 0, got);
                    }
                }
            } finally {
                Marshal.FreeHGlobal(read);
            }
        }
    }
}
'@
}

$image = New-Object -ComObject IMAPI2FS.MsftFileSystemImage
$image.FileSystemsToCreate = 3  # ISO9660 + Joliet
$image.VolumeName = $Label
$image.Root.AddTree((Resolve-Path $Source).Path, $false)
$result = $image.CreateResultImage()
$target = [System.IO.Path]::GetFullPath($Out)
[Ac3Forge.IsoWriter]::Write($target, $result.ImageStream, $result.BlockSize, $result.TotalBlocks)
Write-Host ("wrote {0} ({1:N1} MB, label {2})" -f $target, ((Get-Item $target).Length / 1MB), $Label)
