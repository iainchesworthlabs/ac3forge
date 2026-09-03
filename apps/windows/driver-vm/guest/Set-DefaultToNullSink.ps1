# Runs INSIDE the test guest: makes "Speakers (Desktop Atmos)" the default
# output the way the demo does it, then reports the default. Uses the same
# undocumented policy-config interface the demo's default_device.cpp does,
# here through a tiny inline C# shim, so the driver's endpoint is exercised
# as a default device without the demo itself in the guest.
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
"setting default to $id"
$hr = [DefaultAudio]::Set($id)
"SetDefaultEndpoint hr=0x{0:x8}" -f $hr
