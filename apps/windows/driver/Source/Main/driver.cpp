/*++

DriverEntry and the driver-wide callbacks: a KMDF driver that hands itself
to ACX. Everything device-shaped is in device.cpp.

--*/

#include "nullsink.h"

_Use_decl_annotations_
PAGED_CODE_SEG
VOID
NullSink_EvtDriverUnload(
    _In_ WDFDRIVER Driver
    )
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Driver);
    // Nothing driver-wide is allocated; ACX and WDF clean up their own.
}

INIT_CODE_SEG
extern "C"
NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    WDF_DRIVER_CONFIG       wdfConfig;
    ACX_DRIVER_CONFIG       acxConfig;
    WDF_OBJECT_ATTRIBUTES   attributes;
    WDFDRIVER               driver = nullptr;

    // INIT code is discarded after DriverEntry returns, so there is no
    // paged segment to assert on here (Code Analysis says as much).
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    WDF_DRIVER_CONFIG_INIT(&wdfConfig, NullSink_EvtDeviceAdd);
    wdfConfig.EvtDriverUnload = NullSink_EvtDriverUnload;

    RETURN_NTSTATUS_IF_FAILED(WdfDriverCreate(DriverObject, RegistryPath, &attributes, &wdfConfig, &driver));

    // After the WDF driver object exists, ACX applies its own driver-wide
    // settings; this must precede any device add.
    ACX_DRIVER_CONFIG_INIT(&acxConfig);
    RETURN_NTSTATUS_IF_FAILED(AcxDriverInitialize(driver, &acxConfig));

    return STATUS_SUCCESS;
}
