/*++

The device: created on AddDevice with one render circuit, which joins the
device when its "hardware" is prepared (there is none) and leaves when it
is released. The power policy is the sample's: idle to D3 after a while
with no stream open, D3-cold excluded when the exit latency would be too
long, which for a virtual device it never is.

The sample chains several RETURN_NTSTATUS_IF_FAILED calls in prepare-
hardware; here each is classified. Adding the circuit is required: without
it there is no endpoint, so failing the start is the honest outcome. The
power policy is optional: a device that cannot assign S0 idle settings
still works, so that step logs and continues rather than failing the start
- the lesson of the PortCls driver, where a "harmless" status promoted to a
failure stopped every device start.

--*/

#include "nullsink.h"

namespace {

// The service's Parameters key: the one place a driver that failed to add
// its device can still leave a message. The name is fixed by the INF.
constexpr PCWSTR kParametersKey =
    L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services\\Ac3ForgeNullSink\\Parameters";

// The first note in a chain of returns is the one that names the failing
// call; the callers' notes on the way out would only overwrite it with the
// name of the function that contained it. Cleared when a device is added.
BOOLEAN g_failureNoted = FALSE;

}  // namespace

_Use_decl_annotations_
PAGED_CODE_SEG
VOID
NullSink_NoteFailure(
    _In_ PCWSTR   Step,
    _In_ NTSTATUS Status
    )
{
    PAGED_CODE();

    if (g_failureNoted) {
        return;
    }
    g_failureNoted = TRUE;

    DbgPrintEx(DPFLTR_IHVAUDIO_ID, DPFLTR_ERROR_LEVEL,
               "Ac3ForgeNullSink: %ws failed with 0x%08X\n", Step, static_cast<unsigned>(Status));

    (VOID)RtlCreateRegistryKey(RTL_REGISTRY_ABSOLUTE, const_cast<PWSTR>(kParametersKey));
    (VOID)RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, const_cast<PWSTR>(kParametersKey),
                                L"LastFailedStep", REG_SZ, const_cast<PWSTR>(Step),
                                static_cast<ULONG>((wcslen(Step) + 1) * sizeof(WCHAR)));
    ULONG status = static_cast<ULONG>(Status);
    (VOID)RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, const_cast<PWSTR>(kParametersKey),
                                L"LastFailedStatus", REG_DWORD, &status, sizeof(status));
}

_Use_decl_annotations_
PAGED_CODE_SEG
NTSTATUS
NullSink_EvtDeviceAdd(
    _In_    WDFDRIVER       Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
{
    WDF_OBJECT_ATTRIBUTES           attributes;
    WDF_DEVICE_PNP_CAPABILITIES     pnpCapabilities;
    ACX_DEVICEINIT_CONFIG           deviceInitConfig;
    ACX_DEVICE_CONFIG               deviceConfig;
    WDF_PNPPOWER_EVENT_CALLBACKS    pnpPowerCallbacks;
    WDFDEVICE                       device = nullptr;
    PNULLSINK_DEVICE_CONTEXT        deviceContext = nullptr;

    PAGED_CODE();
    UNREFERENCED_PARAMETER(Driver);

    g_failureNoted = FALSE;

    // Before the device is created, ACX adds its defaults to the init.
    ACX_DEVICEINIT_CONFIG_INIT(&deviceInitConfig);
    NOTE_AND_RETURN_IF_FAILED(AcxDeviceInitInitialize(DeviceInit, &deviceInitConfig));

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);
    pnpPowerCallbacks.EvtDevicePrepareHardware = NullSink_EvtDevicePrepareHardware;
    pnpPowerCallbacks.EvtDeviceReleaseHardware = NullSink_EvtDeviceReleaseHardware;
    pnpPowerCallbacks.EvtDeviceD0Entry = NullSink_EvtDeviceD0Entry;
    pnpPowerCallbacks.EvtDeviceD0Exit = NullSink_EvtDeviceD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPowerCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, NULLSINK_DEVICE_CONTEXT);
    NOTE_AND_RETURN_IF_FAILED(WdfDeviceCreate(&DeviceInit, &attributes, &device));

    deviceContext = GetNullSinkDeviceContext(device);
    deviceContext->Render = nullptr;
    deviceContext->ExcludeD3Cold = WdfFalse;

    // After the device exists, ACX applies its post-creation settings.
    ACX_DEVICE_CONFIG_INIT(&deviceConfig);
    NOTE_AND_RETURN_IF_FAILED(AcxDeviceInitialize(device, &deviceConfig));

    // A virtual device is removed by software; surprise removal is fine.
    WDF_DEVICE_PNP_CAPABILITIES_INIT(&pnpCapabilities);
    pnpCapabilities.SurpriseRemovalOK = WdfTrue;
    WdfDeviceSetPnpCapabilities(device, &pnpCapabilities);

    // The circuit is built now and added to the device in prepare-hardware,
    // the sequence the sample uses.
    NOTE_AND_RETURN_IF_FAILED(NullSink_CreateRenderCircuit(device, &deviceContext->Render));

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
PAGED_CODE_SEG
NTSTATUS
NullSink_EvtDevicePrepareHardware(
    _In_ WDFDEVICE    Device,
    _In_ WDFCMRESLIST ResourceList,
    _In_ WDFCMRESLIST ResourceListTranslated
    )
{
    PNULLSINK_DEVICE_CONTEXT deviceContext = nullptr;

    PAGED_CODE();
    UNREFERENCED_PARAMETER(ResourceList);
    UNREFERENCED_PARAMETER(ResourceListTranslated);

    deviceContext = GetNullSinkDeviceContext(Device);

    // Optional: the device works without an idle policy.
    const NTSTATUS powerStatus = NullSink_ApplyPowerPolicy(Device);
    if (!NT_SUCCESS(powerStatus)) {
        DbgPrintEx(DPFLTR_IHVAUDIO_ID, DPFLTR_WARNING_LEVEL,
                   "Ac3ForgeNullSink: S0 idle settings not applied (0x%08X); continuing without\n",
                   static_cast<unsigned>(powerStatus));
    }

    // Required: this is the endpoint. Not visible until the device is in D0.
    RETURN_NTSTATUS_IF_TRUE(deviceContext->Render == nullptr, STATUS_INVALID_DEVICE_STATE);
    NOTE_AND_RETURN_IF_FAILED(AcxDeviceAddCircuit(Device, deviceContext->Render));

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
PAGED_CODE_SEG
NTSTATUS
NullSink_EvtDeviceReleaseHardware(
    _In_ WDFDEVICE    Device,
    _In_ WDFCMRESLIST ResourceListTranslated
    )
{
    PNULLSINK_DEVICE_CONTEXT deviceContext = nullptr;

    PAGED_CODE();
    UNREFERENCED_PARAMETER(ResourceListTranslated);

    deviceContext = GetNullSinkDeviceContext(Device);
    if (deviceContext->Render != nullptr) {
        // On the way down, a failure here changes nothing the caller can
        // act on; the status is reported, not acted on.
        (VOID)AcxDeviceRemoveCircuit(Device, deviceContext->Render);
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
PAGED_CODE_SEG
NTSTATUS
NullSink_EvtDeviceD0Entry(
    _In_ WDFDEVICE              Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState
    )
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(PreviousState);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
PAGED_CODE_SEG
NTSTATUS
NullSink_EvtDeviceD0Exit(
    _In_ WDFDEVICE              Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState
    )
{
    PAGED_CODE();

    // Going idle to D3 (not a system power action): keep the D3-cold
    // exclusion in step with what ACX says the exit latency must be. A
    // virtual device is always "responsive", so this stays excluded-off,
    // but the check is the sample's and costs nothing.
    if (TargetState == WdfPowerDeviceD3 && WdfDeviceGetSystemPowerAction(Device) == PowerActionNone) {
        PNULLSINK_DEVICE_CONTEXT deviceContext = GetNullSinkDeviceContext(Device);
        const ACX_DX_EXIT_LATENCY latency =
            AcxDeviceGetCurrentDxExitLatency(Device, WdfDeviceGetSystemPowerAction(Device), TargetState);
        const WDF_TRI_STATE excludeD3Cold = (latency == AcxDxExitLatencyResponsive) ? WdfFalse : WdfTrue;
        if (deviceContext->ExcludeD3Cold != excludeD3Cold) {
            deviceContext->ExcludeD3Cold = excludeD3Cold;
            // Optional, as in prepare-hardware.
            (VOID)NullSink_ApplyPowerPolicy(Device);
        }
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
PAGED_CODE_SEG
NTSTATUS
NullSink_ApplyPowerPolicy(
    _In_ WDFDEVICE Device
    )
{
    WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS   idleSettings;
    PNULLSINK_DEVICE_CONTEXT                deviceContext = nullptr;

    PAGED_CODE();

    deviceContext = GetNullSinkDeviceContext(Device);

    WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS_INIT(&idleSettings, IdleCannotWakeFromS0);
    idleSettings.IdleTimeout = NULLSINK_IDLE_TIMEOUT_MS;
    idleSettings.IdleTimeoutType = SystemManagedIdleTimeoutWithHint;
    idleSettings.ExcludeD3Cold = deviceContext->ExcludeD3Cold;

    return WdfDeviceAssignS0IdleSettings(Device, &idleSettings);
}
