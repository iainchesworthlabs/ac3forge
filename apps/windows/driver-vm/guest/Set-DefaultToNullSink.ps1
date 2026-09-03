# Runs INSIDE the test guest: makes "Speakers (Desktop Atmos)" the default
# output the way the demo does it, then reports the default. Uses the same
# undocumented policy-config interface the demo's default_device.cpp does,
# here through a tiny inline C# shim, so the driver's endpoint is exercised
# as a default device without the demo itself in the guest.
#
# -TryFormat <rate> instead asks the endpoint to take a 7.1/16-bit format at
# that sample rate (SetDeviceFormat, the same interface) and reports the
# HRESULT: the driver offers one format and refuses others, so this is the
# set-data-format path under a real request, judged by the report's reader.
param([int]$TryFormat = 0)
$src = @'
using System;
using System.Runtime.InteropServices;
[ComImport, Guid("870af99c-171d-4f9e-af0d-e63df40c2bc9")] class PolicyConfigClient {}
[ComImport, Guid("f8679f50-850a-41cf-9c72-430f290290c8"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IPolicyConfig {
    int GetMixFormat(string id, IntPtr fmt);
    int GetDeviceFormat(string id, int def, IntPtr fmt);
    int ResetDeviceFormat(string id);
    int SetDeviceFormat(string id, IntPtr a, IntPtr b);
    int GetProcessingPeriod(string id, int def, IntPtr a, IntPtr b);
    int SetProcessingPeriod(string id, IntPtr a);
    int GetShareMode(string id, IntPtr m);
    int SetShareMode(string id, IntPtr m);
    int GetPropertyValue(string id, IntPtr key, IntPtr v);
    int SetPropertyValue(string id, IntPtr key, IntPtr v);
    int SetDefaultEndpoint(string id, int role);
    int SetEndpointVisibility(string id, int visible);
}
public static class DefaultAudio {
    public static int Set(string id) {
        var p = (IPolicyConfig)new PolicyConfigClient();
        int hr = 0;
        for (int role = 0; role < 3; ++role) { hr = p.SetDefaultEndpoint(id, role); if (hr != 0) return hr; }
        return hr;
    }
    // A WAVEFORMATEXTENSIBLE: 8 channels, 16-bit, 7.1 surround, at the rate
    // given; SetDeviceFormat takes it twice (endpoint and mix format).
    // Windows PowerShell's Add-Type compiles C# 5: no local functions.
    static void U16(byte[] b, int at, int v) { b[at] = (byte)v; b[at + 1] = (byte)(v >> 8); }
    static void U32(byte[] b, int at, int v) { U16(b, at, v & 0xffff); U16(b, at + 2, (v >> 16) & 0xffff); }
    public static int SetFormat(string id, int rate) {
        var fmt = new byte[40];
        U16(fmt, 0, 0xFFFE); U16(fmt, 2, 8); U32(fmt, 4, rate); U32(fmt, 8, rate * 8 * 2); U16(fmt, 12, 16); U16(fmt, 14, 16); U16(fmt, 16, 22);
        U16(fmt, 18, 16); U32(fmt, 20, 0x63F);
        var pcm = new Guid("00000001-0000-0010-8000-00aa00389b71").ToByteArray();
        Array.Copy(pcm, 0, fmt, 24, 16);
        var mem = Marshal.AllocHGlobal(fmt.Length);
        try {
            Marshal.Copy(fmt, 0, mem, fmt.Length);
            return ((IPolicyConfig)new PolicyConfigClient()).SetDeviceFormat(id, mem, mem);
        } finally { Marshal.FreeHGlobal(mem); }
    }
}
'@
Add-Type -TypeDefinition $src
# The endpoint id lives in the registry under MMDevices; match the friendly name.
$root = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render'
$target = Get-ChildItem $root | Where-Object {
    $props = Get-ItemProperty "$($_.PSPath)\Properties" -ErrorAction SilentlyContinue
    ($props.PSObject.Properties | Where-Object { $_.Value -is [string] -and $_.Value -match 'Desktop Atmos' }).Count -gt 0
} | Select-Object -First 1
if (-not $target) { throw 'no render endpoint named like "Desktop Atmos"' }
$id = '{0.0.0.00000000}.' + $target.PSChildName
if ($TryFormat -gt 0) {
    $hr = [DefaultAudio]::SetFormat($id, $TryFormat)
    "SetDeviceFormat $TryFormat Hz hr=0x{0:x8}" -f $hr
    return
}
"setting default to $id"
$hr = [DefaultAudio]::Set($id)
"SetDefaultEndpoint hr=0x{0:x8}" -f $hr
