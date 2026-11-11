/*
 * viogpu on a Gunyah protected VM: everything the device reads or writes comes from the
 * restricted DMA pool. See viogpu_rdma.h.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "viogpu_rdma.h"

extern "C"
{
#include "rdmaclient.h"
#include <ntstrsafe.h>
}

/* ---- pVM test build: a log in the registry (no debugger, no WPP decoding on the guest) ---- */
#define VIOGPU_REGLOG_BYTES (48 * 1024)
static char g_Log[VIOGPU_REGLOG_BYTES];
static char g_LogFlush[VIOGPU_REGLOG_BYTES];
static ULONG g_LogLen = 0;
static KSPIN_LOCK g_LogLock;
static LONG g_LogLockInit = 0;

extern "C" void VioGpuRegLog(const char *fmt, ...)
{
    char line[320];
    va_list ap;
    if (KeGetCurrentIrql() > DISPATCH_LEVEL)
    {
        return;
    }
    ULONG64 ms = KeQueryInterruptTime() / 10000;
    size_t n = 0;
    RtlStringCbPrintfA(line, sizeof(line), "%I64u ", ms);
    RtlStringCbLengthA(line, sizeof(line), &n);
    va_start(ap, fmt);
    RtlStringCbVPrintfA(line + n, sizeof(line) - n, fmt, ap);
    va_end(ap);
    RtlStringCbLengthA(line, sizeof(line), &n);
    if (n == 0 || line[n - 1] != '\n')
    {
        RtlStringCbCatA(line, sizeof(line), "\n");
        RtlStringCbLengthA(line, sizeof(line), &n);
    }
    DbgPrintEx(DPFLTR_IHVVIDEO_ID, DPFLTR_ERROR_LEVEL, "viogpu: %s", line);

    if (InterlockedCompareExchange(&g_LogLockInit, 1, 0) == 0)
    {
        KeInitializeSpinLock(&g_LogLock);
        InterlockedExchange(&g_LogLockInit, 2);
    }
    while (g_LogLockInit != 2)
    {
        YieldProcessor();
    }
    KIRQL irql;
    KeAcquireSpinLock(&g_LogLock, &irql);
    if (g_LogLen + n > VIOGPU_REGLOG_BYTES)
    {
        /* keep the newest half */
        ULONG keep = VIOGPU_REGLOG_BYTES / 2;
        RtlMoveMemory(g_Log, g_Log + g_LogLen - keep, keep);
        g_LogLen = keep;
    }
    RtlCopyMemory(g_Log + g_LogLen, line, n);
    g_LogLen += (ULONG)n;
    ULONG len = g_LogLen;
    BOOLEAN flush = (irql == PASSIVE_LEVEL);
    if (flush)
    {
        RtlCopyMemory(g_LogFlush, g_Log, len);
    }
    KeReleaseSpinLock(&g_LogLock, irql);
    if (flush)
    {
        RtlWriteRegistryValue(RTL_REGISTRY_SERVICES, L"VioGpuDod", L"DbgLog", REG_BINARY, g_LogFlush, len);
    }
}

#define VIOGPU_RDMA_RING_PAGES 64  /* two vrings with room to spare */
#define VIOGPU_RDMA_SLOT_PAGES 512 /* in-flight commands; 2 MB */
#define VIOGPU_RDMA_MAX_REGIONS 16

extern "C" void VioGpuRegLog(const char *fmt, ...);
#define RDMA_LOG(...) VioGpuRegLog("rdma: " __VA_ARGS__)

typedef struct _VIOGPU_RDMA_REGION
{
    PUCHAR Va;
    PHYSICAL_ADDRESS Pa;
    SIZE_T Bytes;
} VIOGPU_RDMA_REGION;

static RDMA_CLIENT g_Client;
static BOOLEAN g_Tried = FALSE;
static BOOLEAN g_Open = FALSE;
static KSPIN_LOCK g_RegionLock;
static VIOGPU_RDMA_REGION g_Regions[VIOGPU_RDMA_MAX_REGIONS];

static PUCHAR g_RingVa = NULL;
static SIZE_T g_RingUsed = 0;
static PUCHAR g_SlotVa = NULL;
DECLSPEC_ALIGN(MEMORY_ALLOCATION_ALIGNMENT) static SLIST_HEADER g_SlotList;

static PVOID RegionAllocLocked(SIZE_T bytes, PHYSICAL_ADDRESS *pa)
{
    PVOID va = NULL;
    ULONG pages = (ULONG)BYTES_TO_PAGES(bytes);
    NTSTATUS status = RdmaClientAllocRegion(&g_Client, pages, &va, pa);
    if (!NT_SUCCESS(status) || va == NULL)
    {
        RDMA_LOG("region of %lu pages refused (0x%x), pool %I64u bytes\n", pages, status, g_Client.LastPoolTotalSize);
        return NULL;
    }
    KIRQL irql;
    KeAcquireSpinLock(&g_RegionLock, &irql);
    for (int i = 0; i < VIOGPU_RDMA_MAX_REGIONS; i++)
    {
        if (g_Regions[i].Va == NULL)
        {
            g_Regions[i].Pa = *pa;
            g_Regions[i].Bytes = (SIZE_T)pages * PAGE_SIZE;
            g_Regions[i].Va = (PUCHAR)va; /* last: readers check Va */
            KeReleaseSpinLock(&g_RegionLock, irql);
            RtlZeroMemory(va, (SIZE_T)pages * PAGE_SIZE);
            return va;
        }
    }
    KeReleaseSpinLock(&g_RegionLock, irql);
    RdmaClientFreeRegion(&g_Client, va, pages);
    RDMA_LOG("region table full\n");
    return NULL;
}

BOOLEAN VioGpuRdmaOpen(void)
{
    if (g_Tried)
    {
        return g_Open;
    }
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        return FALSE;
    }
    g_Tried = TRUE;
    KeInitializeSpinLock(&g_RegionLock);
    RtlZeroMemory(g_Regions, sizeof(g_Regions));
    InitializeSListHead(&g_SlotList);

    if (!NT_SUCCESS(RdmaClientOpen(&g_Client, "viogpu")))
    {
        RDMA_LOG("no rdmapool, stock DMA paths\n");
        return FALSE;
    }
    PHYSICAL_ADDRESS pa;
    g_RingVa = (PUCHAR)RegionAllocLocked(VIOGPU_RDMA_RING_PAGES * PAGE_SIZE, &pa);
    g_SlotVa = (PUCHAR)RegionAllocLocked(VIOGPU_RDMA_SLOT_PAGES * PAGE_SIZE, &pa);
    if (g_RingVa == NULL || g_SlotVa == NULL)
    {
        RDMA_LOG("cannot reserve ring/slot areas; giving up on the pool\n");
        return FALSE;
    }
    g_Open = TRUE;
    RDMA_LOG("active, pool %I64u bytes\n", g_Client.LastPoolTotalSize);
    VioGpuRdmaReset();
    return TRUE;
}

BOOLEAN VioGpuRdmaActive(void)
{
    return g_Open;
}

void VioGpuRdmaReset(void)
{
    if (!VioGpuRdmaOpen())
    {
        return;
    }
    g_RingUsed = 0;
    RtlZeroMemory(g_RingVa, VIOGPU_RDMA_RING_PAGES * PAGE_SIZE);
    InitializeSListHead(&g_SlotList);
    for (ULONG i = 0; i < VIOGPU_RDMA_SLOT_PAGES; i++)
    {
        InterlockedPushEntrySList(&g_SlotList, (PSLIST_ENTRY)(g_SlotVa + (SIZE_T)i * PAGE_SIZE));
    }
}

PVOID VioGpuRdmaRingAlloc(SIZE_T size)
{
    if (!g_Open)
    {
        return NULL;
    }
    size = ROUND_TO_PAGES(size);
    if (g_RingUsed + size > (SIZE_T)VIOGPU_RDMA_RING_PAGES * PAGE_SIZE)
    {
        RDMA_LOG("ring area exhausted (%Iu + %Iu)\n", g_RingUsed, size);
        return NULL;
    }
    PVOID va = g_RingVa + g_RingUsed;
    g_RingUsed += size;
    RtlZeroMemory(va, size);
    return va;
}

BOOLEAN VioGpuRdmaVaToPa(PVOID va, PHYSICAL_ADDRESS *pa)
{
    if (!g_Open)
    {
        return FALSE;
    }
    PUCHAR p = (PUCHAR)va;
    for (int i = 0; i < VIOGPU_RDMA_MAX_REGIONS; i++)
    {
        PUCHAR base = g_Regions[i].Va;
        if (base != NULL && p >= base && p < base + g_Regions[i].Bytes)
        {
            pa->QuadPart = g_Regions[i].Pa.QuadPart + (LONGLONG)(p - base);
            return TRUE;
        }
    }
    return FALSE;
}

PVOID VioGpuRdmaSlotAlloc(void)
{
    if (!g_Open)
    {
        return NULL;
    }
    PVOID slot = (PVOID)InterlockedPopEntrySList(&g_SlotList);
    if (slot == NULL)
    {
        RDMA_LOG("all %u command slots in flight\n", VIOGPU_RDMA_SLOT_PAGES);
    }
    return slot;
}

void VioGpuRdmaSlotFree(PVOID slot)
{
    if (slot != NULL)
    {
        InterlockedPushEntrySList(&g_SlotList, (PSLIST_ENTRY)slot);
    }
}

PVOID VioGpuRdmaRegionAlloc(SIZE_T bytes, PHYSICAL_ADDRESS *pa)
{
    if (!VioGpuRdmaOpen())
    {
        return NULL;
    }
    return RegionAllocLocked(bytes, pa);
}

void VioGpuRdmaRegionFree(PVOID va)
{
    if (!g_Open || va == NULL)
    {
        return;
    }
    KIRQL irql;
    SIZE_T bytes = 0;
    KeAcquireSpinLock(&g_RegionLock, &irql);
    for (int i = 0; i < VIOGPU_RDMA_MAX_REGIONS; i++)
    {
        if (g_Regions[i].Va == (PUCHAR)va)
        {
            bytes = g_Regions[i].Bytes;
            g_Regions[i].Va = NULL;
            break;
        }
    }
    KeReleaseSpinLock(&g_RegionLock, irql);
    if (bytes)
    {
        RdmaClientFreeRegion(&g_Client, va, (ULONG)(bytes / PAGE_SIZE));
    }
}

void VioGpuRdmaShutdown(void)
{
    if (!g_Tried)
    {
        return;
    }
    if (g_Client.PoolDeviceObject != NULL)
    {
        for (int i = 0; i < VIOGPU_RDMA_MAX_REGIONS; i++)
        {
            if (g_Regions[i].Va != NULL)
            {
                RdmaClientFreeRegion(&g_Client, g_Regions[i].Va, (ULONG)(g_Regions[i].Bytes / PAGE_SIZE));
                g_Regions[i].Va = NULL;
            }
        }
        RdmaClientClose(&g_Client);
    }
    g_RingVa = NULL;
    g_SlotVa = NULL;
    g_Open = FALSE;
    g_Tried = FALSE;
}
