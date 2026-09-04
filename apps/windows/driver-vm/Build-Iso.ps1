# Writes an ISO image from a directory, using Windows' own IMAPI2 (no oscdimg
# or mkisofs needed). Two uses here: small ISO 9660 + Joliet data discs that
# carry autounattend.xml and the driver package into the test guest, and a
# UDF re-pack of the Windows install media with a no-prompt EFI boot image
# (-BootImage), so an unattended guest boots Setup without anyone pressing a
# key.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Source,
    [Parameter(Mandatory)][string]$Out,
    [string]$Label = 'DATA',
    # El Torito EFI boot image (a FAT floppy image such as efisys_noprompt.bin).
    [string]$BootImage,
    # UDF only, for trees with files over 4 GB (install.wim). Default is
    # ISO 9660 + Joliet.
    [switch]$Udf
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Source)) { throw "no such directory: $Source" }
if ($BootImage -and -not (Test-Path $BootImage)) { throw "no such boot image: $BootImage" }

# IMAPI2 hands back a COM IStream; copying it to a file is done in a few
# lines of C# because PowerShell cannot call IStream::Read with the byref
# count the interface wants. Reads are 1 MB at a time: the stream is a
# few blocks for a data disc and four million for install media.
if (-not ('Ac3Forge.IsoWriter' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
namespace Ac3Forge {
    public static class IsoWriter {
        public static long Write(string path, object imageStream) {
            IStream stream = (IStream)imageStream;
            byte[] buffer = new byte[1 << 20];
            IntPtr read = Marshal.AllocHGlobal(4);
            long total = 0;
            try {
                using (FileStream file = new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20)) {
                    for (;;) {
                        stream.Read(buffer, buffer.Length, read);
                        int got = Marshal.ReadInt32(read);
                        if (got <= 0) { break; }
                        file.Write(buffer, 0, got);
                        total += got;
                    }
                }
            } finally {
                Marshal.FreeHGlobal(read);
            }
            return total;
        }
    }
}
'@
}

$image = New-Object -ComObject IMAPI2FS.MsftFileSystemImage
if ($Udf) {
    $image.FileSystemsToCreate = 4          # FsiFileSystemUDF
    $image.UDFRevision = 0x102              # 1.02, what Setup media ships with
} else {
    $image.FileSystemsToCreate = 3          # ISO9660 + Joliet
}
# The default "media" is a CD; tell it the image is a plain file on disk and
# lift the block limit, or a Windows ISO overflows it.
$image.ChooseImageDefaultsForMediaType(12)  # IMAPI_MEDIA_TYPE_DISK
$image.FreeMediaBlocks = 0x7FFFFFFF
$image.VolumeName = $Label

if ($BootImage) {
    $stream = New-Object -ComObject ADODB.Stream
    $stream.Open()
    $stream.Type = 1                         # binary
    $stream.LoadFromFile((Resolve-Path $BootImage).Path)
    $boot = New-Object -ComObject IMAPI2FS.BootOptions
    $boot.AssignBootImage($stream)
    $boot.PlatformId = 0xEF                 # PlatformEFI
    $boot.Emulation = 0                     # EmulationNone
    $boot.Manufacturer = 'Microsoft'
    $image.BootImageOptions = $boot
}

$image.Root.AddTree((Resolve-Path $Source).Path, $false)
$result = $image.CreateResultImage()
$target = [System.IO.Path]::GetFullPath($Out)
$started = Get-Date
$bytes = [Ac3Forge.IsoWriter]::Write($target, $result.ImageStream)
$expected = [long]$result.BlockSize * $result.TotalBlocks
if ($bytes -ne $expected) { throw "short write: $bytes of $expected bytes" }
Write-Host ("wrote {0} ({1:N1} MB, label {2}, {3:N0}s)" -f $target, ($bytes / 1MB), $Label, ((Get-Date) - $started).TotalSeconds)
