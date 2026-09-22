# The Ac3ForgeNullSink device through SetupAPI, for install.ps1 and remove.ps1
# to dot-source. This is the documented sequence for creating a root-
# enumerated device for a driver that has no hardware, and the one the WDK's
# devcon sample performs for "devcon install": a device information element
# in the INF's class, the hardware id the INF matches set on it, the device
# registered (DIF_REGISTERDEVICE), then the driver installed on it
# (UpdateDriverForPlugAndPlayDevices, which also starts it). Everything is
# in setupapi.dll and newdev.dll; nothing needs the WDK. The device is
# ROOT\MEDIA\0000 (or the next free number), as before.
#
# Not SwDeviceCreate: on the Windows 11 25H2 test guest that API returns
# HRESULT 0x8007007E (ERROR_MOD_NOT_FOUND) for every variant tried, from a
# session-0 batch logon and from an elevated desktop task alike, while an
# unelevated call gets the expected access-denied; the cause was not found
# and an install path cannot rest on it.
#
# The functions: Get-NullSinkDevices, Test-NullSinkDevice, New-NullSinkDevice
# (returns the instance id), Restart-NullSinkDevice, Remove-NullSinkDevice.

$script:HardwareId = 'ROOT\Ac3ForgeNullSink'

if (-not ('Ac3Forge.RootDevice' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

namespace Ac3Forge {
    public static class RootDevice {
        [StructLayout(LayoutKind.Sequential)]
        struct SP_DEVINFO_DATA {
            public uint cbSize;
            public Guid ClassGuid;
            public uint DevInst;
            public IntPtr Reserved;
        }

        const uint DICD_GENERATE_ID = 0x1;
        const uint SPDRP_HARDWAREID = 0x1;
        const uint DIF_REGISTERDEVICE = 0x19;
        const uint INSTALLFLAG_FORCE = 0x1;

        [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true, ExactSpelling = true)]
        static extern bool SetupDiGetINFClassW(string InfName, out Guid ClassGuid, StringBuilder ClassName, uint ClassNameSize, out uint RequiredSize);

        [DllImport("setupapi.dll", SetLastError = true, ExactSpelling = true)]
        static extern IntPtr SetupDiCreateDeviceInfoList(ref Guid ClassGuid, IntPtr hwndParent);

        [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true, ExactSpelling = true)]
        static extern bool SetupDiCreateDeviceInfoW(IntPtr DeviceInfoSet, string DeviceName, ref Guid ClassGuid, string DeviceDescription, IntPtr hwndParent, uint CreationFlags, ref SP_DEVINFO_DATA DeviceInfoData);

        [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true, ExactSpelling = true)]
        static extern bool SetupDiSetDeviceRegistryPropertyW(IntPtr DeviceInfoSet, ref SP_DEVINFO_DATA DeviceInfoData, uint Property, byte[] PropertyBuffer, uint PropertyBufferSize);

        [DllImport("setupapi.dll", SetLastError = true, ExactSpelling = true)]
        static extern bool SetupDiCallClassInstaller(uint InstallFunction, IntPtr DeviceInfoSet, ref SP_DEVINFO_DATA DeviceInfoData);

        [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true, ExactSpelling = true)]
        static extern bool SetupDiGetDeviceInstanceIdW(IntPtr DeviceInfoSet, ref SP_DEVINFO_DATA DeviceInfoData, StringBuilder DeviceInstanceId, uint DeviceInstanceIdSize, out uint RequiredSize);

        [DllImport("setupapi.dll", SetLastError = true, ExactSpelling = true)]
        static extern bool SetupDiDestroyDeviceInfoList(IntPtr DeviceInfoSet);

        [DllImport("newdev.dll", CharSet = CharSet.Unicode, SetLastError = true, ExactSpelling = true)]
        static extern bool UpdateDriverForPlugAndPlayDevicesW(IntPtr hwndParent, string HardwareId, string FullInfPath, uint InstallFlags, out bool bRebootRequired);

        static void Check(bool ok, string what) {
            if (!ok) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(), what + " failed");
        }

        // Creates the device node for hardwareId in the INF's class and
        // installs the INF's driver on it. Returns the device instance id
        // and whether Windows asked for a reboot (it does not, for this
        // driver).
        public static string Create(string infPath, string hardwareId, out bool rebootRequired) {
            Guid classGuid;
            var className = new StringBuilder(64);
            uint needed;
            Check(SetupDiGetINFClassW(infPath, out classGuid, className, (uint)className.Capacity, out needed), "SetupDiGetINFClass");

            IntPtr set = SetupDiCreateDeviceInfoList(ref classGuid, IntPtr.Zero);
            Check(set != new IntPtr(-1), "SetupDiCreateDeviceInfoList");
            try {
                var data = new SP_DEVINFO_DATA();
                data.cbSize = (uint)Marshal.SizeOf(typeof(SP_DEVINFO_DATA));
                Check(SetupDiCreateDeviceInfoW(set, className.ToString(), ref classGuid, null, IntPtr.Zero, DICD_GENERATE_ID, ref data), "SetupDiCreateDeviceInfo");

                // A REG_MULTI_SZ of one hardware id.
                var ids = Encoding.Unicode.GetBytes(hardwareId + "\0\0");
                Check(SetupDiSetDeviceRegistryPropertyW(set, ref data, SPDRP_HARDWAREID, ids, (uint)ids.Length), "SetupDiSetDeviceRegistryProperty(HardwareID)");
                Check(SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, ref data), "SetupDiCallClassInstaller(DIF_REGISTERDEVICE)");

                var instance = new StringBuilder(256);
                Check(SetupDiGetDeviceInstanceIdW(set, ref data, instance, (uint)instance.Capacity, out needed), "SetupDiGetDeviceInstanceId");

                Check(UpdateDriverForPlugAndPlayDevicesW(IntPtr.Zero, hardwareId, infPath, INSTALLFLAG_FORCE, out rebootRequired), "UpdateDriverForPlugAndPlayDevices");
                return instance.ToString();
            } finally {
                SetupDiDestroyDeviceInfoList(set);
            }
        }
    }
}
'@
}

function Get-NullSinkDevices {
    # Every device node carrying the hardware id, present or not (a removed
    # node can linger as a phantom until its package is deleted).
    Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains $script:HardwareId }
}

function Test-NullSinkDevice {
    [bool](Get-NullSinkDevices | Where-Object Present)
}

function New-NullSinkDevice([Parameter(Mandatory)][string]$Inf) {
    $reboot = $false
    $instance = [Ac3Forge.RootDevice]::Create((Resolve-Path $Inf).Path, $script:HardwareId, [ref]$reboot)
    if ($reboot) { Write-Warning 'Windows asked for a reboot to finish the install' }
    $instance
}

function Restart-NullSinkDevice {
    foreach ($device in (Get-NullSinkDevices | Where-Object Present)) {
        $device | Disable-PnpDevice -Confirm:$false
        $device | Enable-PnpDevice -Confirm:$false
    }
}

function Remove-NullSinkDevice {
    # pnputil's device removal (Windows 10 2004 and later): the same
    # DIF_REMOVE the class installer performs for devcon remove.
    foreach ($device in Get-NullSinkDevices) {
        & pnputil /remove-device $device.InstanceId | Out-Null
    }
}
