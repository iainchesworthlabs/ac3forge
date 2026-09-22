/*++

Pool-backed operator new and delete, the sample's shape with one change
carried over from the PortCls driver: the executable pool flag is masked
off whatever the caller asked for (C28160 pointed at the sample passing
flags straight through). Nothing this driver allocates is code, and HVCI
refuses executable pool anyway.

--*/

#include "nullsink.h"

PVOID
operator new(
    _In_ size_t     size,
    _In_ POOL_FLAGS poolFlags,
    _In_ ULONG      tag
    )
{
#pragma warning(suppress: 28160)  // the mask below is what keeps the pool non-executable
    return ExAllocatePool2(poolFlags & ~POOL_FLAG_NON_PAGED_EXECUTE, size, tag);
}

void __cdecl
operator delete(
    _Pre_maybenull_ __drv_freesMem(Mem) PVOID buffer
    )
{
    if (buffer != nullptr) {
        ExFreePool(buffer);
    }
}

void __cdecl
operator delete(
    _Pre_maybenull_ __drv_freesMem(Mem) PVOID buffer,
    _In_ size_t size
    )
{
    UNREFERENCED_PARAMETER(size);
    if (buffer != nullptr) {
        ExFreePool(buffer);
    }
}
